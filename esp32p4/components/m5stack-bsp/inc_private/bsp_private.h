/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Implementation-only conveniences for the BSP's own .c files (heavy includes
 * + helper macros). Device drivers should include "bsp_types.h" instead — they
 * don't need esp_log / stdio just to declare their API.
 */

#pragma once
#include <stdio.h>
#include <assert.h>
#include "bsp_types.h"
#include "esp_log.h"

#define BSP_ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#define BSP_RETURN_ERR(e) do { if (e != ESP_OK) return e; } while (0)
