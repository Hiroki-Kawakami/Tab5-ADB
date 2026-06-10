// Host (simulator) implementation of jpeg_decode_enhanced Layer 1 — the
// whole-frame path (jpeg_enh_decoder_process) only. The device variant replays
// the IDF decode flow through 2D-DMA strip descriptors; on the host the
// idf_compat libjpeg shim decodes the whole frame in one call. libjpeg decodes
// JFIF full-range, so yuv_full_range is implicit and the simulator reproduces
// the device's full-range colours. Strip mode (ring_count > 0) has no host
// consumer and is rejected at handle creation.

#include "jpeg_decode_enhanced.h"

#include <stdlib.h>
#include <string.h>

struct jpeg_enh_strip_decoder_s {
    jpeg_enh_strip_decoder_cfg_t cfg;
    jpeg_decoder_handle_t dec;
};

esp_err_t jpeg_enh_strip_decoder_new(const jpeg_enh_strip_decoder_cfg_t *cfg,
                                     jpeg_enh_strip_decoder_handle_t *out_handle)
{
    if (!cfg || !out_handle) return ESP_ERR_INVALID_ARG;
    if (cfg->max_pic_w == 0 || cfg->max_pic_h == 0) return ESP_ERR_INVALID_ARG;
    if (cfg->ring_count > 0) return ESP_ERR_NOT_SUPPORTED;  // whole-frame only on host

    struct jpeg_enh_strip_decoder_s *h = calloc(1, sizeof(*h));
    if (!h) return ESP_ERR_NO_MEM;
    h->cfg = *cfg;

    jpeg_decode_engine_cfg_t ec = {
        .intr_priority = cfg->intr_priority,
        .timeout_ms = cfg->timeout_ms ? cfg->timeout_ms : 200,
    };
    esp_err_t err = jpeg_new_decoder_engine(&ec, &h->dec);
    if (err != ESP_OK) { free(h); return err; }

    *out_handle = h;
    return ESP_OK;
}

esp_err_t jpeg_enh_strip_decoder_del(jpeg_enh_strip_decoder_handle_t h)
{
    if (!h) return ESP_OK;
    if (h->dec) jpeg_del_decoder_engine(h->dec);
    free(h);
    return ESP_OK;
}

esp_err_t jpeg_enh_decoder_process(jpeg_enh_strip_decoder_handle_t h,
                                   const uint8_t *bit_stream, uint32_t stream_size,
                                   void *out_buf, size_t out_buf_size,
                                   jpeg_enh_frame_info_t *info)
{
    if (!h || !bit_stream || !stream_size || !out_buf) return ESP_ERR_INVALID_ARG;

    jpeg_decode_picture_info_t pi;
    esp_err_t err = jpeg_decoder_get_info(bit_stream, stream_size, &pi);
    if (err != ESP_OK) return err;
    if (pi.width > h->cfg.max_pic_w || pi.height > h->cfg.max_pic_h)
        return ESP_ERR_INVALID_SIZE;

    jpeg_decode_cfg_t dc = {
        .output_format = h->cfg.decode.output_format,
        .rgb_order = h->cfg.decode.rgb_order,
        .conv_std = h->cfg.decode.conv_std,
    };
    uint32_t out_size = 0;
    err = jpeg_decoder_process(h->dec, &dc, bit_stream, stream_size,
                               out_buf, out_buf_size, &out_size);
    if (err != ESP_OK) return err;

    if (info) {
        memset(info, 0, sizeof(*info));
        info->origin_w = info->pic_w = pi.width;
        info->origin_h = info->pic_h = pi.height;
        info->mcu_w = info->mcu_h = 16;
        info->strip_h = pi.height;
        info->strip_count = 1;
    }
    return ESP_OK;
}

// Strip-mode surface: handles are always created with ring_count = 0 on the
// host, so these can only be reached through misuse.
esp_err_t jpeg_enh_strip_decoder_process(jpeg_enh_strip_decoder_handle_t h,
                                         const uint8_t *bit_stream, uint32_t stream_size,
                                         jpeg_enh_frame_info_t *info)
{
    (void)h; (void)bit_stream; (void)stream_size; (void)info;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t jpeg_enh_strip_decoder_release_strip(jpeg_enh_strip_decoder_handle_t h,
                                               uint32_t strip_idx)
{
    (void)h; (void)strip_idx;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t jpeg_enh_strip_decoder_sync_strip_for_cpu(jpeg_enh_strip_decoder_handle_t h,
                                                    const jpeg_enh_strip_event_t *evt)
{
    (void)h; (void)evt;
    return ESP_ERR_NOT_SUPPORTED;
}

uint32_t jpeg_enh_strip_decoder_strips_delivered(jpeg_enh_strip_decoder_handle_t h)
{
    (void)h;
    return 0;
}

size_t jpeg_enh_strip_decoder_strip_buffer_size(jpeg_enh_strip_decoder_handle_t h)
{
    (void)h;
    return 0;
}
