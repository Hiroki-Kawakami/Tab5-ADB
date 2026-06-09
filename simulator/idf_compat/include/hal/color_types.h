// Host (simulator) shim for hal/color_types.h. Per the idf_compat rule this is
// an Espressif-defined API, so the host mirrors the IDF header *itself* (pure
// type definitions, no I/O) rather than re-abstracting it. Pulled in transitively
// by driver/ppa.h via hal/ppa_types.h — the PPA op configs reference the
// color_pixel_* pixel structs and the COLOR_TYPE_ID-derived color-mode ids
// directly, so the values must match IDF for source portability. Kept in sync
// with ESP-IDF v5.4.3 hal/color_types.h.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Color Space
 *
 * @note Save enum 0 for special purpose
 */
typedef enum {
    COLOR_SPACE_RAW = 1,     ///< Color space raw
    COLOR_SPACE_RGB,         ///< Color space rgb
    COLOR_SPACE_YUV,         ///< Color space yuv
    COLOR_SPACE_GRAY,        ///< Color space gray
    COLOR_SPACE_ARGB,        ///< Color space argb
    COLOR_SPACE_ALPHA,       ///< Color space alpha (A)
    COLOR_SPACE_CLUT,        ///< Color look-up table (L)
} color_space_t;

/**
 * @brief RGB Format
 */
typedef enum {
    COLOR_PIXEL_RGB888,      ///< 24 bits, 8 bits per R/G/B value
    COLOR_PIXEL_RGB666,      ///< 18 bits, 6 bits per R/G/B value
    COLOR_PIXEL_RGB565,      ///< 16 bits, 5 bits per R/B value, 6 bits for G value
} color_pixel_rgb_format_t;

/**
 * @brief YUV Format
 */
typedef enum {
    COLOR_PIXEL_YUV444,    ///< 24 bits, 8 bits per Y/U/V value
    COLOR_PIXEL_YUV422,    ///< 16 bits, 8-bit Y per pixel, 8-bit U and V per two pixels
    COLOR_PIXEL_YUV420,    ///< 12 bits, 8-bit Y per pixel, 8-bit U and V per four pixels
    COLOR_PIXEL_YUV411,    ///< 12 bits, 8-bit Y per pixel, 8-bit U and V per four pixels
} color_pixel_yuv_format_t;

/**
 * @brief ARGB Format
 */
typedef enum {
    COLOR_PIXEL_ARGB8888,   ///< 32 bits, 8 bits per A(alpha)/R/G/B value
} color_pixel_argb_format_t;

/**
 * @brief Alpha(A) Format
 */
typedef enum {
    COLOR_PIXEL_A4,   ///< 4 bits, opacity only
    COLOR_PIXEL_A8,   ///< 8 bits, opacity only
} color_pixel_alpha_format_t;

///< Bitwidth of the `color_space_pixel_format_t::color_space` field
#define COLOR_SPACE_BITWIDTH                 8
///< Bitwidth of the `color_space_pixel_format_t::pixel_format` field
#define COLOR_PIXEL_FORMAT_BITWIDTH          24
///< Helper to get the color_space from a unique color type ID
#define COLOR_SPACE_TYPE(color_type_id)      (((color_type_id) >> COLOR_PIXEL_FORMAT_BITWIDTH) & ((1 << COLOR_SPACE_BITWIDTH) - 1))
///< Helper to get the pixel_format from a unique color type ID
#define COLOR_PIXEL_FORMAT(color_type_id)    ((color_type_id) & ((1 << COLOR_PIXEL_FORMAT_BITWIDTH) - 1))
///< Make a unique ID of a color based on the value of color space and pixel format
#define COLOR_TYPE_ID(color_space, pixel_format) (((color_space) << COLOR_PIXEL_FORMAT_BITWIDTH) | (pixel_format))

/**
 * @brief Color Space Info Structure
 */
typedef union {
    struct {
        uint32_t pixel_format: COLOR_PIXEL_FORMAT_BITWIDTH;    ///< Format of a certain color space type
        uint32_t color_space: COLOR_SPACE_BITWIDTH;            ///< Color space type
    };
    uint32_t color_type_id;                                    ///< Unique type of a certain color pixel format
} color_space_pixel_format_t;

/**
 * @brief Color range
 */
typedef enum {
    COLOR_RANGE_LIMIT, /*!< Limited color range, 16 is the darkest black and 235 is the brightest white */
    COLOR_RANGE_FULL,  /*!< Full color range, 0 is the darkest black and 255 is the brightest white */
} color_range_t;

/**
 * @brief The standard used for conversion between RGB and YUV
 */
typedef enum {
    COLOR_CONV_STD_RGB_YUV_BT601, /*!< YUV<->RGB conversion standard: BT.601 */
    COLOR_CONV_STD_RGB_YUV_BT709, /*!< YUV<->RGB conversion standard: BT.709 */
} color_conv_std_rgb_yuv_t;

/**
 * @brief RGB element order
 */
typedef enum {
    COLOR_RGB_ELEMENT_ORDER_RGB, /*!< RGB element order: RGB */
    COLOR_RGB_ELEMENT_ORDER_BGR, /*!< RGB element order: BGR */
} color_rgb_element_order_t;

/**
 * @brief Data structure for ARGB8888 pixel unit
 */
typedef union {
    struct {
        uint32_t b: 8;      /*!< B component [0, 255] */
        uint32_t g: 8;      /*!< G component [0, 255] */
        uint32_t r: 8;      /*!< R component [0, 255] */
        uint32_t a: 8;      /*!< A component [0, 255] */
    };
    uint32_t val;           /*!< 32-bit ARGB8888 value */
} color_pixel_argb8888_data_t;

/**
 * @brief Data structure for RGB888 pixel unit
 */
typedef struct {
    uint8_t b;      /*!< B component [0, 255] */
    uint8_t g;      /*!< G component [0, 255] */
    uint8_t r;      /*!< R component [0, 255] */
} color_pixel_rgb888_data_t;

/**
 * @brief Data structure for RGB565 pixel unit
 */
typedef union {
    struct {
        uint16_t b: 5;      /*!< B component [0, 31] */
        uint16_t g: 6;      /*!< G component [0, 63] */
        uint16_t r: 5;      /*!< R component [0, 31] */
    };
    uint16_t val;           /*!< 16-bit RGB565 value */
} color_pixel_rgb565_data_t;

#ifdef __cplusplus
}
#endif
