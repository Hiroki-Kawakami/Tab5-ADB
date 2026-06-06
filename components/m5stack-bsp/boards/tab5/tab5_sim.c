/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * M5Stack Tab5 simulator board: the host-side counterpart of tab5.c. It maps
 * the same public bsp_init()/bsp_restart() onto the reusable SDL backend
 * (simulator/sdl_backend), passing Tab5's geometry (720x1280 portrait) and
 * pixel format. The build selects tab5.c on device and tab5_sim.c on the
 * simulator (see ../../CMakeLists.txt). Audio/Wi-Fi/BT have no host backend
 * yet — they will be added here when the simulator needs them.
 */

#include "bsp.h"
#include "bsp_display.h"
#include "bsp_touch.h"
#include "sdl_backend.h"

#include <stdlib.h>

esp_err_t bsp_init(const bsp_config_t *config) {
    sdl_backend_config_t sdl_config = {
        .title     = "Tab5 ADB Simulator",
        .size      = { 720, 1280 },
        .format    = config->display.pixel_format,
        .fb_num    = config->display.fb_num ? config->display.fb_num : 1,
        .scale_div = 2,
    };

    bsp_display_t *display = NULL;
    bsp_touch_t   *touch   = NULL;
    esp_err_t err = sdl_backend_create(&sdl_config, &display, &touch);
    if (err != ESP_OK) return err;

    bsp_display_set_active(display);
    bsp_touch_set_active(touch);
    return ESP_OK;
}

void bsp_restart(void) {
    exit(0);
}
