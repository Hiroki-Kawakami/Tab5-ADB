/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Sim harness implementation (see sim_harness.h). A tiny line-oriented script
 * interpreter that runs on the main/LVGL thread: it pumps lv_timer_handler
 * between steps (draining async work and animations into the BSP framebuffer)
 * and snapshots the completed frame from the SDL backend to a JPEG. JPEG keeps
 * the dependency to libjpeg (already linked into the simulator) and is readable
 * by image tools; the BSP backend itself stays image-format agnostic, only
 * handing out the raw framebuffer via sdl_backend_snapshot().
 */

#include "sim_harness.h"

#include "lvgl.h"
#include "sdl_backend.h"
#include "wifi_manager.hpp"
#include "wifi_sim.hpp"

#include <SDL2/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include <jpeglib.h>
}

namespace {

/* Parse a wifi::Result name (for the wifi-connect-result command). */
bool parse_wifi_result(const char *s, wifi::Result &out) {
    using R = wifi::Result;
    struct { const char *name; R r; } map[] = {
        {"Ok", R::Ok}, {"ApNotFound", R::ApNotFound}, {"AuthFailed", R::AuthFailed},
        {"AssocFailed", R::AssocFailed}, {"IpFailed", R::IpFailed},
        {"Timeout", R::Timeout}, {"Failed", R::Failed},
    };
    for (auto &m : map) if (strcmp(s, m.name) == 0) { out = m.r; return true; }
    return false;
}

/* Parse "ssid:rssi:secured,ssid:rssi:secured,..." into a fake AP list. */
std::vector<wifi::AP> parse_wifi_aps(const char *spec) {
    std::vector<wifi::AP> aps;
    std::string s(spec);
    size_t i = 0;
    while (i < s.size()) {
        size_t comma = s.find(',', i);
        std::string entry = s.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
        size_t c1 = entry.find(':'), c2 = entry.rfind(':');
        if (c1 != std::string::npos && c2 != c1) {
            wifi::AP ap;
            ap.ssid = entry.substr(0, c1);
            ap.rssi = (int8_t)atoi(entry.substr(c1 + 1, c2 - c1 - 1).c_str());
            ap.secured = atoi(entry.substr(c2 + 1).c_str()) != 0;
            aps.push_back(ap);
        }
        if (comma == std::string::npos) break;
        i = comma + 1;
    }
    return aps;
}

constexpr uint32_t FRAME_MS = 16;   /* ~60 Hz pump step */

/* Run one LVGL service iteration and sleep roughly one frame. */
void pump_once() {
    lv_timer_handler();
    SDL_Delay(FRAME_MS);
}

/* Pump the LVGL loop for ms milliseconds (wall clock). */
void pump_for(uint32_t ms) {
    uint32_t start = SDL_GetTicks();
    do {
        pump_once();
    } while (SDL_GetTicks() - start < ms);
}

/* Pump until no animation is running for a few consecutive frames, or max_ms
 * elapses. Drains lv_async_call work (scheduled as one-shot LVGL timers) and
 * settles in-flight animations so the captured frame is final. */
void settle(uint32_t max_ms) {
    uint32_t start = SDL_GetTicks();
    int quiet = 0;
    while (SDL_GetTicks() - start < max_ms) {
        pump_once();
        quiet = (lv_anim_count_running() == 0) ? quiet + 1 : 0;
        if (quiet >= 4) return;   /* ~4 quiet frames */
    }
}

/* Encode the most recently flushed framebuffer to a JPEG at path. */
bool capture(const char *path) {
    int w = 0, h = 0;
    bsp_pixel_format_t fmt = BSP_PIXEL_FORMAT_RGB565;
    const void *fb = sdl_backend_snapshot(&w, &h, &fmt);
    if (!fb || w <= 0 || h <= 0) {
        fprintf(stderr, "[sim] capture: no framebuffer\n");
        return false;
    }

    std::error_code ec;
    std::filesystem::path out(path);
    if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path(), ec);

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[sim] capture: cannot open %s\n", path);
        return false;
    }

    jpeg_compress_struct cinfo;
    jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, f);
    cinfo.image_width = (JDIMENSION)w;
    cinfo.image_height = (JDIMENSION)h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 90, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    std::vector<uint8_t> row((size_t)w * 3);
    const size_t stride = (size_t)w * (fmt == BSP_PIXEL_FORMAT_RGB888 ? 3 : 2);
    while (cinfo.next_scanline < (JDIMENSION)h) {
        const uint8_t *src = (const uint8_t *)fb + (size_t)cinfo.next_scanline * stride;
        if (fmt == BSP_PIXEL_FORMAT_RGB888) {
            // The framebuffer holds B,G,R (LVGL native); JCS_RGB wants R,G,B.
            for (int x = 0; x < w; x++) {
                row[x * 3 + 0] = src[x * 3 + 2];
                row[x * 3 + 1] = src[x * 3 + 1];
                row[x * 3 + 2] = src[x * 3 + 0];
            }
        } else {
            const uint16_t *p = (const uint16_t *)src;   /* RGB565, native order */
            for (int x = 0; x < w; x++) {
                uint16_t v = p[x];
                uint8_t r5 = (v >> 11) & 0x1F, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
                row[x * 3 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
                row[x * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
                row[x * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
            }
        }
        JSAMPROW rp = row.data();
        jpeg_write_scanlines(&cinfo, &rp, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(f);
    fprintf(stderr, "[sim] captured %s (%dx%d)\n", path, w, h);
    return true;
}

/* A press/release must each span at least one LVGL indev read for the click to
 * register, so a tap holds and waits a few frames on each edge. */
constexpr uint32_t TOUCH_HOLD_MS = 80;

/* Execute one trimmed, non-empty script line. Returns false to stop the run. */
bool run_line(char *line) {
    char cmd[32] = {0};
    char a1[1024] = {0}, a2[1024] = {0};
    int n = sscanf(line, "%31s %1023s %1023s", cmd, a1, a2);
    if (n < 1) return true;
    if (cmd[0] == '#') return true;

    if (strcmp(cmd, "quit") == 0) {
        return false;
    } else if (strcmp(cmd, "wait") == 0) {
        pump_for(n >= 2 ? (uint32_t)atoi(a1) : 0);
    } else if (strcmp(cmd, "settle") == 0) {
        settle(n >= 2 ? (uint32_t)atoi(a1) : 5000);
    } else if (strcmp(cmd, "capture") == 0) {
        if (n >= 2) capture(a1);
        else fprintf(stderr, "[sim] capture: missing path\n");
    } else if (strcmp(cmd, "tap") == 0) {
        if (n >= 3) {
            sdl_backend_inject_down(atoi(a1), atoi(a2));
            pump_for(TOUCH_HOLD_MS);
            sdl_backend_inject_up();
            pump_for(TOUCH_HOLD_MS);
        } else {
            fprintf(stderr, "[sim] tap: need x y\n");
        }
    } else if (strcmp(cmd, "down") == 0 || strcmp(cmd, "move") == 0) {
        if (n >= 3) sdl_backend_inject_down(atoi(a1), atoi(a2));
        else fprintf(stderr, "[sim] %s: need x y\n", cmd);
    } else if (strcmp(cmd, "up") == 0) {
        sdl_backend_inject_up();
    } else if (strcmp(cmd, "wifi-aps") == 0) {
        if (n >= 2) wifi::sim::set_aps(parse_wifi_aps(a1));
        else fprintf(stderr, "[sim] wifi-aps: need ssid:rssi:secured,...\n");
    } else if (strcmp(cmd, "wifi-connect-result") == 0) {
        wifi::Result r;
        if (n >= 2 && parse_wifi_result(a1, r)) wifi::sim::set_next_connect_result(r);
        else fprintf(stderr, "[sim] wifi-connect-result: bad/missing result name\n");
    } else if (strcmp(cmd, "wifi-delay") == 0) {
        if (n >= 2) wifi::sim::set_event_delay_ms(atoi(a1));
        else fprintf(stderr, "[sim] wifi-delay: need ms\n");
    } else if (strcmp(cmd, "wifi-drop") == 0) {
        wifi::sim::drop_link();
    } else {
        fprintf(stderr, "[sim] unknown command: %s\n", cmd);
    }
    return true;
}

}  // namespace

int sim_harness_run(const char *script_path) {
    FILE *f = (strcmp(script_path, "-") == 0) ? stdin : fopen(script_path, "r");
    if (!f) {
        fprintf(stderr, "[sim] cannot open script: %s\n", script_path);
        return 1;
    }

    char line[1100];
    while (fgets(line, sizeof(line), f)) {
        if (!run_line(line)) break;
    }
    if (f != stdin) fclose(f);
    return 0;
}
