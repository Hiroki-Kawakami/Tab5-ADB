/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Model-agnostic touch layer: holds the active bsp_touch provider (registered by
 * the board's bsp_init via bsp_touch_set_active) and implements the public
 * bsp_touch_* API by dispatching through its vtable. Shared by every board.
 */

#include "bsp.h"
#include "bsp_touch.h"

static bsp_touch_t *s_touch;

void bsp_touch_set_active(bsp_touch_t *touch) {
    s_touch = touch;
}

int bsp_touch_read(bsp_touch_point_t *points, uint8_t max_points) {
    return s_touch ? s_touch->read(s_touch, points, max_points) : 0;
}

void bsp_touch_wait_interrupt(void) {
    if (s_touch) s_touch->wait_interrupt(s_touch);
}
