/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "bsp_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create an ST7123 I2C touch panel as a bsp_touch provider. */
BSP_NONNULL(1, 2) esp_err_t st7123_touch_create(const bsp_touch_config_t *config, bsp_touch_t **out);

#ifdef __cplusplus
}
#endif
