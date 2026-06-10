// Host (simulator) shim for the ESP-IDF JPEG decode engine API — the subset the
// app's mirror-render path uses. The device build pulls the real header from
// esp_driver_jpeg; here it is backed by libjpeg (see src/jpeg_decode.c).
//
// Per the idf_compat rule: this is an Espressif-defined API, so the host
// implements the IDF API *itself* rather than re-abstracting it. The full-range
// YUV->RGB fix the device needs (jpeg_decode_enhanced) is a 2D-DMA CSC artifact;
// libjpeg already decodes JFIF full-range, so on the host that component's sim
// build just delegates to jpeg_decoder_process() here.
//
// Only the decoder surface is shimmed (no encoder). Enum *values* are
// shim-local (the device uses IDF's COLOR_TYPE_ID-derived ids); app code uses
// the constant *names*, which match IDF, so it stays source-portable.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPEG_DECODE_OUT_FORMAT_RGB888,
    JPEG_DECODE_OUT_FORMAT_RGB565,
    JPEG_DECODE_OUT_FORMAT_GRAY,
    JPEG_DECODE_OUT_FORMAT_YUV444,
    JPEG_DECODE_OUT_FORMAT_YUV422,
    JPEG_DECODE_OUT_FORMAT_YUV420,
} jpeg_dec_output_format_t;

typedef enum {
    JPEG_YUV_RGB_CONV_STD_BT601,
    JPEG_YUV_RGB_CONV_STD_BT709,
} jpeg_yuv_rgb_conv_std_t;

typedef enum {
    JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
} jpeg_dec_rgb_element_order_t;

typedef enum {
    JPEG_DEC_ALLOC_INPUT_BUFFER = 0,
    JPEG_DEC_ALLOC_OUTPUT_BUFFER = 1,
} jpeg_dec_buffer_alloc_direction_t;

typedef enum {
    JPEG_DOWN_SAMPLING_YUV444,
    JPEG_DOWN_SAMPLING_YUV422,
    JPEG_DOWN_SAMPLING_YUV420,
    JPEG_DOWN_SAMPLING_GRAY,
} jpeg_down_sampling_type_t;

typedef struct jpeg_decoder_t *jpeg_decoder_handle_t;

typedef struct {
    jpeg_dec_output_format_t output_format;
    jpeg_dec_rgb_element_order_t rgb_order;
    jpeg_yuv_rgb_conv_std_t conv_std;
} jpeg_decode_cfg_t;

typedef struct {
    int intr_priority;
    int timeout_ms;
} jpeg_decode_engine_cfg_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    jpeg_down_sampling_type_t sample_method;
} jpeg_decode_picture_info_t;

typedef struct {
    jpeg_dec_buffer_alloc_direction_t buffer_direction;
} jpeg_decode_memory_alloc_cfg_t;

esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t *dec_eng_cfg,
                                  jpeg_decoder_handle_t *ret_decoder);

esp_err_t jpeg_decoder_get_info(const uint8_t *bit_stream, uint32_t stream_size,
                                jpeg_decode_picture_info_t *picture_info);

esp_err_t jpeg_decoder_process(jpeg_decoder_handle_t decoder_engine,
                               const jpeg_decode_cfg_t *decode_cfg,
                               const uint8_t *bit_stream, uint32_t stream_size,
                               uint8_t *decode_outbuf, uint32_t outbuf_size,
                               uint32_t *out_size);

esp_err_t jpeg_del_decoder_engine(jpeg_decoder_handle_t decoder_engine);

void *jpeg_alloc_decoder_mem(size_t size,
                             const jpeg_decode_memory_alloc_cfg_t *mem_cfg,
                             size_t *allocated_size);

#ifdef __cplusplus
}
#endif
