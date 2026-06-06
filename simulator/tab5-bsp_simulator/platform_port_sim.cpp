// Host (SDL2) implementation of the `pf_port` platform abstraction used by the
// Tab5 app. Mirrors the on-device implementation in idf-components/main/main.cpp
// so app/ code (adb_app, screens) compiles and runs unchanged on the desktop.
//
// The Tab5 panel is 720x1280 portrait RGB565. The app sets up its own LVGL
// display bound to the two frame buffers returned by display_get_frame_buffer()
// and pushes finished frames with display_flush(); here those buffers are plain
// host memory and display_flush() blits them to an SDL window.

#include "platform_port.hpp"
#include "adb_app.hpp"
#include "lvgl.h"

#include <SDL2/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <tuple>
#include <vector>

namespace {

// Native panel geometry (see adb_app.hpp PANEL_W/PANEL_H). The window is shown
// at half scale so the 720x1280 portrait panel fits on a laptop screen.
constexpr int kPanelW = PANEL_W;
constexpr int kPanelH = PANEL_H;
constexpr int kWindowScaleDiv = 2;

SDL_Window   *s_window;
SDL_Renderer *s_renderer;
SDL_Texture  *s_texture;        // RGB565, panel-sized

std::vector<uint16_t> s_fb[2];  // two RGB565 frame buffers (DIRECT mode)
lv_timer_t           *s_event_timer;

std::optional<std::tuple<int, int>> s_touch;

std::tuple<int, int> window_to_panel(int wx, int wy) {
    int ww = kPanelW / kWindowScaleDiv, wh = kPanelH / kWindowScaleDiv;
    if (s_window) SDL_GetWindowSize(s_window, &ww, &wh);
    if (ww <= 0) ww = kPanelW;
    if (wh <= 0) wh = kPanelH;
    int x = (int)((int64_t)wx * kPanelW / ww);
    int y = (int)((int64_t)wy * kPanelH / wh);
    if (x < 0) x = 0; else if (x > kPanelW - 1) x = kPanelW - 1;
    if (y < 0) y = 0; else if (y > kPanelH - 1) y = kPanelH - 1;
    return std::make_tuple(x, y);
}

void event_pump(lv_timer_t *) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                std::exit(0);
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    s_touch = window_to_panel(ev.button.x, ev.button.y);
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    s_touch.reset();
                break;
            case SDL_MOUSEMOTION:
                if (s_touch.has_value())
                    s_touch = window_to_panel(ev.motion.x, ev.motion.y);
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_LEAVE)
                    s_touch.reset();
                break;
            default:
                break;
        }
    }
}

}  // namespace

namespace pf_port {

PixelFormat display_pixel_format() {
    return PixelFormat::RGB565;
}

void init(int fb_num, PixelFormat /*pixel_format*/) {
    (void)fb_num;
    if (s_window) return;

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init(VIDEO): %s\n", SDL_GetError());
        std::exit(1);
    }
    lv_tick_set_cb(SDL_GetTicks);
    lv_delay_set_cb(SDL_Delay);

    s_window = SDL_CreateWindow("Tab5 ADB Simulator",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                kPanelW / kWindowScaleDiv, kPanelH / kWindowScaleDiv,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    s_renderer = SDL_CreateRenderer(s_window, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_RGB565,
                                  SDL_TEXTUREACCESS_STREAMING, kPanelW, kPanelH);
    if (!s_window || !s_renderer || !s_texture) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        std::exit(1);
    }

    s_fb[0].assign((size_t)kPanelW * kPanelH, 0x0000);
    s_fb[1].assign((size_t)kPanelW * kPanelH, 0x0000);

    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 0xFF);
    SDL_RenderClear(s_renderer);
    SDL_RenderPresent(s_renderer);

    if (!s_event_timer)
        s_event_timer = lv_timer_create(event_pump, 5, nullptr);
}

void display_set_brightness(int /*value*/) {
    // No-op on host.
}

void *display_get_frame_buffer(int fb_index) {
    return s_fb[fb_index & 1].data();
}

void display_flush(int fb_index) {
    if (!s_texture) return;
    SDL_UpdateTexture(s_texture, nullptr,
                      s_fb[fb_index & 1].data(), kPanelW * (int)sizeof(uint16_t));
    SDL_RenderCopy(s_renderer, s_texture, nullptr, nullptr);
    SDL_RenderPresent(s_renderer);
}

std::optional<std::tuple<int, int>> touch_get_point() {
    return s_touch;
}

void *psram_malloc(size_t size)     { return malloc(size); }
void *psram_malloc_dma(size_t size) { return malloc(size); }

}  // namespace pf_port

// On device, app_main lives in idf-components/main/main.cpp (not compiled for
// the host). Provide the host entry point that hands off to the shared app.
extern "C" void app_main(void) {
    adb_app();
}
