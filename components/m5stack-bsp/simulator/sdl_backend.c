/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * SDL implementation of the BSP simulator backend (see sdl_backend.h). Mirrors
 * the device drivers' provider model: it fills a bsp_display_t / bsp_touch_t
 * vtable backed by an SDL window so app code reaches it through the same
 * bsp_display_* / bsp_touch_* API on host and device.
 *
 * Only one SDL window per process is supported (the host runs a single board),
 * so the SDL state and the two provider instances are file-static.
 */

#include "sdl_backend.h"

#include <SDL2/SDL.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static SDL_Window   *s_window;
static SDL_Renderer *s_renderer;
static SDL_Texture  *s_texture;

static int                s_panel_w;
static int                s_panel_h;
static int                s_scale_div = 1;
static size_t             s_bpp = 2;          /* bytes per pixel */
static bsp_pixel_format_t s_format = BSP_PIXEL_FORMAT_RGB565;

static uint8_t *s_fb[2];
static void    *s_fb_ptrs[2];

/* Headless verification support (driven by the sim harness). When the
 * SIMULATOR_HEADLESS env var is set we skip the SDL window/renderer/texture
 * entirely — no host display, so it runs in non-interactive shells / CI and
 * never touches the host screen. LVGL still renders straight into s_fb in
 * DIRECT mode, so a finished frame is fully present without any window;
 * s_last_flushed tracks which of the two buffers holds the most recently
 * completed frame so a snapshot reads the right one. */
static bool s_headless;
static int  s_last_flushed;
/* Set by display_flush (any thread); the actual SDL render is deferred to
 * sdl_backend_present() on the main thread. */
static bool s_dirty;

/* Synthetic touch for the sim harness. In headless mode there is no mouse, so
 * touch_read reports this injected pointer instead. State is touched only from
 * the main/LVGL thread (the harness pumps lv_timer_handler, which calls
 * touch_read), so no locking is needed. */
static bool s_inject_pressed;
static int  s_inject_x;
static int  s_inject_y;

static bsp_display_t s_display;
static bsp_touch_t   s_touch;

static Uint32 sdl_pixel_format(bsp_pixel_format_t fmt) {
    return (fmt == BSP_PIXEL_FORMAT_RGB888) ? SDL_PIXELFORMAT_RGB24
                                            : SDL_PIXELFORMAT_RGB565;
}

static void window_to_panel(int wx, int wy, int *px, int *py) {
    int ww = s_panel_w / s_scale_div, wh = s_panel_h / s_scale_div;
    if (s_window) SDL_GetWindowSize(s_window, &ww, &wh);
    if (ww <= 0) ww = s_panel_w;
    if (wh <= 0) wh = s_panel_h;
    int x = (int)((int64_t)wx * s_panel_w / ww);
    int y = (int)((int64_t)wy * s_panel_h / wh);
    if (x < 0) x = 0; else if (x > s_panel_w - 1) x = s_panel_w - 1;
    if (y < 0) y = 0; else if (y > s_panel_h - 1) y = s_panel_h - 1;
    *px = x;
    *py = y;
}

/* Drain the SDL event queue. Touch is sampled directly from the mouse state in
 * touch_read, so the only event we must act on here is window close. */
static void pump_events(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) exit(0);
    }
}

/* MARK: bsp_display vtable */

static esp_err_t display_draw_bitmap(bsp_display_t *self, bsp_rect_t area, const void *pixels) {
    (void)self;
    if (!s_texture) return ESP_ERR_INVALID_STATE;
    SDL_Rect r = { area.origin.x, area.origin.y, area.size.width, area.size.height };
    SDL_UpdateTexture(s_texture, &r, pixels, area.size.width * (int)s_bpp);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);
    return ESP_OK;
}

static esp_err_t display_deinit(bsp_display_t *self) {
    (void)self;
    return ESP_OK;
}

static void **display_get_framebuffers(bsp_display_t *self) {
    (void)self;
    return s_fb_ptrs;
}

/* Record the latest completed frame and mark it for presentation. The actual SDL
 * render is deferred to sdl_backend_present() on the MAIN thread, because SDL /
 * Cocoa rendering is main-thread-only on macOS — a background producer (e.g. the
 * mirror decode task) must be able to flush without touching SDL. */
static esp_err_t display_flush(bsp_display_t *self, int fb_index) {
    (void)self;
    s_last_flushed = fb_index & 1;   /* recorded even headless, for snapshots */
    s_dirty = true;
    return ESP_OK;
}

static esp_err_t display_set_brightness(bsp_display_t *self, int brightness) {
    (void)self;
    (void)brightness;
    return ESP_OK;  /* No backlight on host. */
}

/* MARK: bsp_touch vtable */

static int touch_read(bsp_touch_t *self, bsp_touch_point_t *points, uint8_t max_points) {
    (void)self;
    if (max_points == 0) return 0;

    /* Headless: no mouse — report the harness's injected pointer (already in
     * panel coordinates, just clamped). */
    if (s_headless) {
        if (!s_inject_pressed) return 0;
        int x = s_inject_x, y = s_inject_y;
        if (x < 0) x = 0; else if (x > s_panel_w - 1) x = s_panel_w - 1;
        if (y < 0) y = 0; else if (y > s_panel_h - 1) y = s_panel_h - 1;
        points[0].x = x;
        points[0].y = y;
        points[0].strength = 1;
        return 1;
    }

    pump_events();
    int wx, wy;
    Uint32 buttons = SDL_GetMouseState(&wx, &wy);
    if (!(buttons & SDL_BUTTON(SDL_BUTTON_LEFT))) return 0;
    window_to_panel(wx, wy, &points[0].x, &points[0].y);
    points[0].strength = 1;
    return 1;
}

static void touch_wait_interrupt(bsp_touch_t *self) {
    (void)self;
    SDL_Delay(5);
}

static esp_err_t touch_deinit(bsp_touch_t *self) {
    (void)self;
    return ESP_OK;
}

esp_err_t sdl_backend_create(const sdl_backend_config_t *config,
                             bsp_display_t **out_display,
                             bsp_touch_t **out_touch) {
    if (!config || !out_display || !out_touch) return ESP_ERR_INVALID_ARG;
    if (s_window) {  /* already created — single window per process */
        *out_display = &s_display;
        *out_touch = &s_touch;
        return ESP_OK;
    }

    s_panel_w   = config->size.width;
    s_panel_h   = config->size.height;
    s_scale_div = config->scale_div > 0 ? config->scale_div : 1;
    s_bpp       = bsp_pixel_format_bytes(config->format);
    s_format    = config->format;
    s_headless  = getenv("SIMULATOR_HEADLESS") != NULL;

    SDL_SetMainReady();
    /* Headless still needs the SDL timer (LVGL's tick source is SDL_GetTicks)
     * but no video subsystem — no window pops up and no display is required. */
    Uint32 init_flags = s_headless ? SDL_INIT_TIMER : SDL_INIT_VIDEO;
    if (SDL_Init(init_flags) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return ESP_FAIL;
    }

    if (!s_headless) {
        s_window = SDL_CreateWindow(config->title ? config->title : "BSP Simulator",
                                    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    s_panel_w / s_scale_div, s_panel_h / s_scale_div,
                                    SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
        s_renderer = SDL_CreateRenderer(s_window, -1,
                                        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        s_texture = SDL_CreateTexture(s_renderer, sdl_pixel_format(config->format),
                                      SDL_TEXTUREACCESS_STREAMING, s_panel_w, s_panel_h);
        if (!s_window || !s_renderer || !s_texture) {
            fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
            return ESP_FAIL;
        }
    }

    int fb_num = config->fb_num ? config->fb_num : 1;
    if (fb_num > 2) fb_num = 2;
    size_t fb_bytes = (size_t)s_panel_w * s_panel_h * s_bpp;
    for (int i = 0; i < 2; i++) {
        s_fb[i] = calloc(1, fb_bytes);
        if (!s_fb[i]) return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < 2; i++) {
        s_fb_ptrs[i] = (i < fb_num) ? s_fb[i] : s_fb[0];
    }

    if (s_renderer) {
        SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 0xFF);
        SDL_RenderClear(s_renderer);
        SDL_RenderPresent(s_renderer);
    }

    s_display.size             = config->size;
    s_display.format           = config->format;
    s_display.draw_bitmap      = display_draw_bitmap;
    s_display.deinit           = display_deinit;
    s_display.get_framebuffers = display_get_framebuffers;
    s_display.flush            = display_flush;
    s_display.set_brightness   = display_set_brightness;

    s_touch.read           = touch_read;
    s_touch.wait_interrupt = touch_wait_interrupt;
    s_touch.deinit         = touch_deinit;

    *out_display = &s_display;
    *out_touch = &s_touch;
    return ESP_OK;
}

void sdl_backend_present(void) {
    if (s_headless || !s_texture || !s_dirty) return;
    s_dirty = false;
    SDL_UpdateTexture(s_texture, NULL, s_fb[s_last_flushed & 1], s_panel_w * (int)s_bpp);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);
}

void sdl_backend_inject_down(int x, int y) {
    s_inject_x = x;
    s_inject_y = y;
    s_inject_pressed = true;
}

void sdl_backend_inject_up(void) {
    s_inject_pressed = false;
}

const void *sdl_backend_snapshot(int *width, int *height, bsp_pixel_format_t *format) {
    if (!s_fb[0]) return NULL;   /* backend not created yet */
    if (width)  *width  = s_panel_w;
    if (height) *height = s_panel_h;
    if (format) *format = s_format;
    return s_fb[s_last_flushed & 1];
}
