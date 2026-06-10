/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Shared vocabulary for the BSP and its internal device drivers: geometry
 * primitives, pixel format, esp_err, and the nonnull annotation. This is the
 * one header a device driver needs to speak the BSP's types — it deliberately
 * pulls in nothing heavy (no esp_log / stdio).
 */

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_NONNULL(...) __attribute__((nonnull(__VA_ARGS__)))

typedef struct { int x, y; } bsp_point_t;
typedef struct { int width, height; } bsp_size_t;

typedef struct { bsp_point_t origin; bsp_size_t size; } bsp_rect_t;
static inline int bsp_rect_width(bsp_rect_t rect) { return rect.size.width; }
static inline int bsp_rect_height(bsp_rect_t rect) { return rect.size.height; }
static inline int bsp_rect_min_x(bsp_rect_t rect) { return rect.origin.x; }
static inline int bsp_rect_min_y(bsp_rect_t rect) { return rect.origin.y; }
static inline int bsp_rect_max_x(bsp_rect_t rect) { return rect.origin.x + rect.size.width; }
static inline int bsp_rect_max_y(bsp_rect_t rect) { return rect.origin.y + rect.size.height; }

typedef enum {
    BSP_PIXEL_FORMAT_RGB565,
    BSP_PIXEL_FORMAT_RGB888,
} bsp_pixel_format_t;

static inline size_t bsp_pixel_format_bytes(bsp_pixel_format_t format) {
    return (format == BSP_PIXEL_FORMAT_RGB888) ? 3 : 2;
}

typedef struct {
    int x, y;
    int strength;   /*!< touch pressure; 0 when the controller doesn't report it */
    int id;         /*!< pointer track id from the touch controller (stable across a
                         gesture, for multi-touch). 0 when the controller/backend
                         doesn't report it (e.g. the single-point simulator). */
} bsp_touch_point_t;

#ifdef __cplusplus
}
#endif
