/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Model-agnostic display layer: holds the active bsp_display provider (registered
 * by the board's bsp_init via bsp_display_set_active) and implements the public
 * bsp_display_* API by dispatching through its vtable. Shared by every board, so
 * a new board only creates a provider — it never re-implements this glue.
 */

#include "bsp.h"
#include "bsp_display.h"

static bsp_display_t *s_display;
static void **s_frame_buffers;   /* cached from get_framebuffers() at registration */

void bsp_display_set_active(bsp_display_t *display) {
    s_display = display;
    s_frame_buffers = (display && display->get_framebuffers)
        ? display->get_framebuffers(display)
        : NULL;
}

void bsp_display_set_brightness(int brightness) {
    if (s_display && s_display->set_brightness) s_display->set_brightness(s_display, brightness);
}

void *bsp_display_get_frame_buffer(int fb_index) {
    return s_frame_buffers ? s_frame_buffers[fb_index] : NULL;
}

void bsp_display_flush(int fb_index) {
    if (s_display && s_display->flush) s_display->flush(s_display, fb_index);
}
