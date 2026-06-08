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

#include <SDL2/SDL.h>

#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <jpeglib.h>
}

namespace {

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
            memcpy(row.data(), src, (size_t)w * 3);
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

/* Execute one trimmed, non-empty script line. Returns false to stop the run. */
bool run_line(char *line) {
    char cmd[32] = {0};
    char arg[1024] = {0};
    int n = sscanf(line, "%31s %1023s", cmd, arg);
    if (n < 1) return true;
    if (cmd[0] == '#') return true;

    if (strcmp(cmd, "quit") == 0) {
        return false;
    } else if (strcmp(cmd, "wait") == 0) {
        pump_for(n >= 2 ? (uint32_t)atoi(arg) : 0);
    } else if (strcmp(cmd, "settle") == 0) {
        settle(n >= 2 ? (uint32_t)atoi(arg) : 5000);
    } else if (strcmp(cmd, "capture") == 0) {
        if (n >= 2) capture(arg);
        else fprintf(stderr, "[sim] capture: missing path\n");
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
