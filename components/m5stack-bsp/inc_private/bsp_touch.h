/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Internal touch-driver interface. Same struct-inheritance vtable shape as
 * bsp_display: a driver embeds bsp_touch_t as its first member and returns
 * &state->base from its *_create(). Coordinates are reported as the BSP's own
 * bsp_touch_point_t so no esp_lcd_touch types leak past the driver.
 */

#pragma once
#include "bsp_types.h"

/* bsp_touch_config_t carries the device-side bus/GPIO wiring a real touch
 * controller needs, so it pulls in IDF driver headers — device-only. The
 * portable vtable below (struct bsp_touch + bsp_touch_set_active) has no such
 * dependency, so a simulator backend can implement it without the driver layer. */
#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/i2c_master.h"

typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    bsp_size_t              size;
    gpio_num_t              int_gpio;
    gpio_num_t              rst_gpio;
    uint32_t                scl_speed_hz;
    bool                    interrupt;
} bsp_touch_config_t;
#endif

typedef struct bsp_touch bsp_touch_t;

struct bsp_touch {
    int       (*read)(bsp_touch_t *self, bsp_touch_point_t *points, uint8_t max_points);
    void      (*wait_interrupt)(bsp_touch_t *self);
    esp_err_t (*deinit)(bsp_touch_t *self);
};

/* Register the active touch panel with the common layer (src/bsp_touch.c), which
 * implements the model-agnostic public bsp_touch_* API on top of it. A board's
 * bsp_init() calls this once after creating its touch provider. */
void bsp_touch_set_active(bsp_touch_t *touch);
