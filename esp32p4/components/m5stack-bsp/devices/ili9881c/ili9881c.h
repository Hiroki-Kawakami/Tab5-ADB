/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "bsp_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create an ILI9881C MIPI-DSI panel as a bsp_display provider. */
BSP_NONNULL(1, 2) esp_err_t ili9881c_lcd_create(const bsp_display_config_t *config, bsp_display_t **out);

#ifdef __cplusplus
}
#endif
