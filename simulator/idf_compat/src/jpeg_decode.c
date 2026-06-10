// Host implementation of the ESP-IDF JPEG decode engine API (driver/jpeg_decode.h),
// backed by libjpeg. The device runs the P4 HW JPEG codec; here we decode on the
// CPU. libjpeg's YCbCr->RGB is full-range (JFIF) — which is exactly what the
// device's jpeg_decode_enhanced produces with yuv_full_range via its CSC-matrix
// override — so the simulator faithfully previews the full-range device output.
//
// Only RGB565 / RGB888 output is supported (the panel is RGB565); GRAY/YUV
// outputs return ESP_ERR_NOT_SUPPORTED. rgb_order selects R/B channel order;
// RGB565 is packed as a host-native uint16 (5-6-5, R in the high bits), matching
// what the app blits into the bsp framebuffer.

#include "driver/jpeg_decode.h"

#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

struct jpeg_decoder_t {
    int timeout_ms;  // accepted, unused on the host (no async DMA)
};

// libjpeg's default error_exit calls exit(); route errors through setjmp so a
// corrupt strip fails the call instead of killing the simulator.
struct sim_jpeg_err {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};

static void sim_jpeg_error_exit(j_common_ptr cinfo)
{
    struct sim_jpeg_err *err = (struct sim_jpeg_err *)cinfo->err;
    longjmp(err->jb, 1);
}

esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t *dec_eng_cfg,
                                  jpeg_decoder_handle_t *ret_decoder)
{
    if (!ret_decoder) {
        return ESP_ERR_INVALID_ARG;
    }
    struct jpeg_decoder_t *eng = calloc(1, sizeof(*eng));
    if (!eng) {
        return ESP_ERR_NO_MEM;
    }
    eng->timeout_ms = dec_eng_cfg ? dec_eng_cfg->timeout_ms : -1;
    *ret_decoder = eng;
    return ESP_OK;
}

esp_err_t jpeg_del_decoder_engine(jpeg_decoder_handle_t decoder_engine)
{
    if (!decoder_engine) {
        return ESP_ERR_INVALID_ARG;
    }
    free(decoder_engine);
    return ESP_OK;
}

void *jpeg_alloc_decoder_mem(size_t size,
                             const jpeg_decode_memory_alloc_cfg_t *mem_cfg,
                             size_t *allocated_size)
{
    (void)mem_cfg;  // host has no DMA-alignment constraint
    void *p = malloc(size);
    if (p && allocated_size) {
        *allocated_size = size;
    }
    return p;
}

esp_err_t jpeg_decoder_get_info(const uint8_t *bit_stream, uint32_t stream_size,
                                jpeg_decode_picture_info_t *picture_info)
{
    if (!bit_stream || !picture_info) {
        return ESP_ERR_INVALID_ARG;
    }

    struct jpeg_decompress_struct cinfo;
    struct sim_jpeg_err err;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = sim_jpeg_error_exit;
    if (setjmp(err.jb)) {
        jpeg_destroy_decompress(&cinfo);
        return ESP_ERR_INVALID_ARG;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, bit_stream, stream_size);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return ESP_ERR_INVALID_ARG;
    }

    picture_info->width = cinfo.image_width;
    picture_info->height = cinfo.image_height;
    // The agent streams YUV420; report it (informational only on the host).
    picture_info->sample_method = JPEG_DOWN_SAMPLING_YUV420;

    jpeg_destroy_decompress(&cinfo);
    return ESP_OK;
}

esp_err_t jpeg_decoder_process(jpeg_decoder_handle_t decoder_engine,
                               const jpeg_decode_cfg_t *decode_cfg,
                               const uint8_t *bit_stream, uint32_t stream_size,
                               uint8_t *decode_outbuf, uint32_t outbuf_size,
                               uint32_t *out_size)
{
    if (!decoder_engine || !decode_cfg || !bit_stream || !decode_outbuf || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t bpp;
    switch (decode_cfg->output_format) {
    case JPEG_DECODE_OUT_FORMAT_RGB565: bpp = 2; break;
    case JPEG_DECODE_OUT_FORMAT_RGB888: bpp = 3; break;
    default:
        return ESP_ERR_NOT_SUPPORTED;  // panel is RGB; GRAY/YUV not used
    }
    const bool bgr = (decode_cfg->rgb_order == JPEG_DEC_RGB_ELEMENT_ORDER_BGR);

    struct jpeg_decompress_struct cinfo;
    struct sim_jpeg_err err;
    uint8_t *rowbuf = NULL;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = sim_jpeg_error_exit;
    if (setjmp(err.jb)) {
        free(rowbuf);
        jpeg_destroy_decompress(&cinfo);
        return ESP_ERR_INVALID_STATE;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, bit_stream, stream_size);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return ESP_ERR_INVALID_ARG;
    }
    cinfo.out_color_space = JCS_RGB;  // full-range JFIF YCbCr->RGB (matches the device's full-range CSC)
    jpeg_start_decompress(&cinfo);

    const uint32_t w = cinfo.output_width;
    const uint32_t h = cinfo.output_height;
    const uint32_t need = w * h * bpp;
    if (need > outbuf_size) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return ESP_ERR_INVALID_ARG;
    }

    rowbuf = malloc((size_t)w * cinfo.output_components);  // RGB888 scanline
    if (!rowbuf) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return ESP_ERR_NO_MEM;
    }

    while (cinfo.output_scanline < h) {
        uint32_t row = cinfo.output_scanline;
        JSAMPROW p = rowbuf;
        jpeg_read_scanlines(&cinfo, &p, 1);
        const uint8_t *s = rowbuf;
        if (bpp == 2) {
            // RGB565 byte-order quirk: the Tab5 panel is driven R-in-high-bits, and
            // on the P4 HW JPEG decoder that packing is produced by rgb_order == BGR
            // (the RGB enum's 565 scramble corrupts the 16-bit pixel). So mirror that
            // here — BGR maps to the R-high packing — to preview the device-correct
            // colours. (The RGB888 branch below keeps the naive byte order.)
            uint16_t *d = (uint16_t *)(decode_outbuf + (size_t)row * w * 2);
            for (uint32_t x = 0; x < w; x++, s += 3) {
                uint8_t r = bgr ? s[0] : s[2];
                uint8_t g = s[1];
                uint8_t b = bgr ? s[2] : s[0];
                d[x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
            }
        } else {
            uint8_t *d = decode_outbuf + (size_t)row * w * 3;
            for (uint32_t x = 0; x < w; x++, s += 3, d += 3) {
                d[0] = bgr ? s[2] : s[0];
                d[1] = s[1];
                d[2] = bgr ? s[0] : s[2];
            }
        }
    }

    free(rowbuf);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    *out_size = need;
    return ESP_OK;
}
