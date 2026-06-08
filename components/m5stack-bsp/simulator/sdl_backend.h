/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Reusable SDL host backend for the BSP simulator boards. This is the
 * simulator-side analogue of devices/ (reusable chip drivers): it turns an SDL
 * window into a bsp_display + bsp_touch provider so every model's simulator
 * board can share the same SDL plumbing (window/texture blit, event pump,
 * mouse->touch mapping). Only what differs per model — window title, panel
 * geometry, pixel format, frame-buffer count, on-screen scale — is passed in
 * via sdl_backend_config_t; a board (boards/<model>/<model>_sim.cpp) calls
 * sdl_backend_create() and registers the returned providers.
 */

#pragma once
#include "bsp_types.h"
#include "bsp_display.h"
#include "bsp_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char         *title;        /*!< window title */
    bsp_size_t          size;         /*!< native panel geometry (px) */
    bsp_pixel_format_t  format;       /*!< framebuffer pixel format */
    uint8_t             fb_num;       /*!< number of frame buffers (1 or 2) */
    int                 scale_div;    /*!< window shown at size / scale_div (>=1) */
} sdl_backend_config_t;

/* Bring up the SDL window and return display + touch providers backed by it.
 * Both providers share the one window; register them with
 * bsp_display_set_active() / bsp_touch_set_active().
 *
 * When the SIMULATOR_HEADLESS env var is set, no window/renderer is created
 * (offscreen framebuffers only) so the simulator runs without a host display —
 * used by the sim harness for automated UI verification. */
esp_err_t sdl_backend_create(const sdl_backend_config_t *config,
                             bsp_display_t **out_display,
                             bsp_touch_t **out_touch);

/* Return a pointer to the most recently flushed framebuffer (the completed
 * frame) plus its geometry / pixel format, for the sim harness to snapshot.
 * The pointer is owned by the backend; valid until the next flush. Returns
 * NULL if the backend has not been created. Call from the LVGL/main thread
 * after the frame has settled so the buffer holds a complete image. */
const void *sdl_backend_snapshot(int *width, int *height, bsp_pixel_format_t *format);

#ifdef __cplusplus
}
#endif
