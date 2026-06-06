/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "esp_err.h"
#include "misc/bsp_display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BSP_WIFI_MODE_NONE = 0,
    BSP_WIFI_MODE_STA  = 1 << 0,
    BSP_WIFI_MODE_AP   = 1 << 1,
} bsp_wifi_mode_t;

#ifdef __cplusplus
}
#endif
