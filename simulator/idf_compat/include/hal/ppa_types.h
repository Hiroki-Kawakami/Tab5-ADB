// Host (simulator) shim for hal/ppa_types.h. Per the idf_compat rule the PPA is
// an Espressif-defined API, so the host mirrors the IDF type header verbatim
// (pure enums, no I/O) — the color-mode ids are COLOR_TYPE_ID-derived so they
// match the device, keeping app code source-portable. Kept in sync with
// ESP-IDF v5.4.3 hal/ppa_types.h.
#pragma once

#include <stdint.h>
#include "hal/color_types.h"
#include "hal/dma2d_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enumeration of engines in PPA modules
 */
typedef enum {
    PPA_ENGINE_TYPE_SRM,            /*!< PPA Scaling-Rotating-Mirroring (SRM) engine */
    PPA_ENGINE_TYPE_BLEND,          /*!< PPA Blending engine, used to perform blend or fill */
} ppa_engine_type_t;

/**
 * @brief Enumeration of PPA Scaling-Rotating-Mirroring available rotation angle (counterclockwise)
 */
typedef enum {
    PPA_SRM_ROTATION_ANGLE_0,        /*!< Picture does no rotation */
    PPA_SRM_ROTATION_ANGLE_90,       /*!< Picture rotates 90 degrees CCW */
    PPA_SRM_ROTATION_ANGLE_180,      /*!< Picture rotates 180 degrees CCW */
    PPA_SRM_ROTATION_ANGLE_270,      /*!< Picture rotates 270 degrees CCW */
} ppa_srm_rotation_angle_t;

/**
 * @brief Enumeration of PPA Scaling-Rotating-Mirroring available color mode
 */
typedef enum {
    PPA_SRM_COLOR_MODE_ARGB8888 = COLOR_TYPE_ID(COLOR_SPACE_ARGB, COLOR_PIXEL_ARGB8888),      /*!< ARGB8888 */
    PPA_SRM_COLOR_MODE_RGB888 = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB888),           /*!< RGB888 */
    PPA_SRM_COLOR_MODE_RGB565 = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565),           /*!< RGB565 */
    PPA_SRM_COLOR_MODE_YUV420 = COLOR_TYPE_ID(COLOR_SPACE_YUV, COLOR_PIXEL_YUV420),           /*!< YUV420 */
    PPA_SRM_COLOR_MODE_YUV444 = COLOR_TYPE_ID(COLOR_SPACE_YUV, COLOR_PIXEL_YUV444),           /*!< YUV444 (limited range only) */
} ppa_srm_color_mode_t;

/**
 * @brief Enumeration of PPA blend available color mode
 */
typedef enum {
    PPA_BLEND_COLOR_MODE_ARGB8888 = COLOR_TYPE_ID(COLOR_SPACE_ARGB, COLOR_PIXEL_ARGB8888),   /*!< ARGB8888 */
    PPA_BLEND_COLOR_MODE_RGB888 = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB888),        /*!< RGB888 */
    PPA_BLEND_COLOR_MODE_RGB565 = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565),        /*!< RGB565 */
    PPA_BLEND_COLOR_MODE_A8 = COLOR_TYPE_ID(COLOR_SPACE_ALPHA, COLOR_PIXEL_A8),              /*!< A8, foreground only */
    PPA_BLEND_COLOR_MODE_A4 = COLOR_TYPE_ID(COLOR_SPACE_ALPHA, COLOR_PIXEL_A4),              /*!< A4, foreground only */
} ppa_blend_color_mode_t;

/**
 * @brief Enumeration of PPA fill available color mode
 */
typedef enum {
    PPA_FILL_COLOR_MODE_ARGB8888 = COLOR_TYPE_ID(COLOR_SPACE_ARGB, COLOR_PIXEL_ARGB8888),    /*!< ARGB8888 */
    PPA_FILL_COLOR_MODE_RGB888 = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB888),         /*!< RGB888 */
    PPA_FILL_COLOR_MODE_RGB565 = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565),         /*!< RGB565 */
} ppa_fill_color_mode_t;

/**
 * @brief Enumeration of PPA alpha compositing update mode
 */
typedef enum {
    PPA_ALPHA_NO_CHANGE = 0,  /*!< Do not replace alpha value (A' = A). If no alpha info, 255 is used. */
    PPA_ALPHA_FIX_VALUE,      /*!< Replace the alpha value with a new, fixed alpha value (A' = val) */
    PPA_ALPHA_SCALE,          /*!< Scale the alpha value (A' = (A * val) >> 8). If no alpha info, A = 255. */
    PPA_ALPHA_INVERT,         /*!< Invert the alpha value (A' = 255 - A). If no alpha info, A' = 0. */
} ppa_alpha_update_mode_t;

/**
 * @brief Enumeration of PPA supported color conversion standard between RGB and YUV
 */
typedef enum {
    PPA_COLOR_CONV_STD_RGB_YUV_BT601 = COLOR_CONV_STD_RGB_YUV_BT601,      /*!< BT.601 */
    PPA_COLOR_CONV_STD_RGB_YUV_BT709 = COLOR_CONV_STD_RGB_YUV_BT709,      /*!< BT.709 */
} ppa_color_conv_std_rgb_yuv_t;

/**
 * @brief Enumeration of PPA supported color range
 */
typedef enum {
    PPA_COLOR_RANGE_LIMIT = COLOR_RANGE_LIMIT,      /*!< Limited color range */
    PPA_COLOR_RANGE_FULL = COLOR_RANGE_FULL,        /*!< Full color range */
} ppa_color_range_t;

/**
 * @brief Enumeration of PPA supported data burst length
 */
typedef enum {
    PPA_DATA_BURST_LENGTH_8 = DMA2D_DATA_BURST_LENGTH_8,        /*!< Data burst length: 8 bytes */
    PPA_DATA_BURST_LENGTH_16 = DMA2D_DATA_BURST_LENGTH_16,      /*!< Data burst length: 16 bytes */
    PPA_DATA_BURST_LENGTH_32 = DMA2D_DATA_BURST_LENGTH_32,      /*!< Data burst length: 32 bytes */
    PPA_DATA_BURST_LENGTH_64 = DMA2D_DATA_BURST_LENGTH_64,      /*!< Data burst length: 64 bytes */
    PPA_DATA_BURST_LENGTH_128 = DMA2D_DATA_BURST_LENGTH_128,    /*!< Data burst length: 128 bytes */
} ppa_data_burst_length_t;

#ifdef __cplusplus
}
#endif
