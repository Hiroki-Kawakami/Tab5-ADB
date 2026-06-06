/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Internal display-driver interface. A driver allocates a struct whose FIRST
 * member is a bsp_display_t (struct-inheritance vtable), fills the function
 * pointers, and returns &state->base from its *_create(). The board then calls
 * through the members directly (single indirection: disp->op(disp, ...)).
 *
 * The portable base contract is draw_bitmap (blit a rectangle of pixels).
 * Host-side framebuffers (get_framebuffers + flush) and a controllable
 * backlight (set_brightness) are optional fast paths: a driver leaves the
 * corresponding pointer NULL when the panel has no such capability (e.g. an EPD
 * has no host framebuffer and no backlight). This keeps the seam usable for
 * non-MIPI panels (SPI-with-GRAM, EPD) without baking the framebuffer-swap
 * model into the contract.
 */

#pragma once
#include "bsp_types.h"

typedef struct {
    int                backlight_gpio;
    bsp_size_t         size;
    bsp_pixel_format_t pixel_format;
    uint8_t            fb_num;
} bsp_display_config_t;

typedef struct bsp_display bsp_display_t;

struct bsp_display {
    /* descriptor — filled by the driver */
    bsp_size_t          size;
    bsp_pixel_format_t  format;

    /* portable base contract (always non-NULL) */
    esp_err_t (*draw_bitmap)(bsp_display_t *self, bsp_rect_t area, const void *pixels);
    esp_err_t (*deinit)(bsp_display_t *self);

    /* host-side framebuffer fast path — NULL when the panel has no host FB */
    void   ** (*get_framebuffers)(bsp_display_t *self);
    esp_err_t (*flush)(bsp_display_t *self, int fb_index);

    /* backlight — NULL when the panel has no controllable backlight */
    esp_err_t (*set_brightness)(bsp_display_t *self, int brightness);
};

/* Register the active display with the common layer (src/bsp_display.c), which
 * implements the model-agnostic public bsp_display_* API on top of it. A board's
 * bsp_init() calls this once after creating its display provider. */
void bsp_display_set_active(bsp_display_t *display);
