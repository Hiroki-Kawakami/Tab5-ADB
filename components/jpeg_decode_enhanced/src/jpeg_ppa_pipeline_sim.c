// Host (simulator) implementation of jpeg_decode_enhanced Layer 2
// (jpeg_ppa_pipeline). The device variant strip-pipelines the P4 HW JPEG
// decoder through 2D-DMA into a ring of strip buffers + PPA; none of that HW
// exists on the host. Here we just decode the whole frame with the idf_compat
// libjpeg shim, then run one software-PPA scale/rotate/mirror into the output —
// the strip ring is purely a device-side memory optimisation, so a full-frame
// decode reproduces the same pixels. The libjpeg shim already decodes JFIF
// full-range, so yuv_full_range is implicit (see jpeg_fullrange_decode_sim.c).
//
// Only Layer 2 is implemented (the app uses jpeg_ppa_pipeline_*); the Layer 1
// jpeg_enh_* strip API has no host consumer.

#include "jpeg_ppa_pipeline.h"

#include <stdlib.h>
#include <string.h>

#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "jpeg_ppa_sim";

struct jpeg_ppa_pipeline_s {
    jpeg_ppa_pipeline_cfg_t cfg;
    jpeg_decoder_handle_t dec;
    ppa_client_handle_t ppa;
    uint8_t *full;       // full-frame decode scratch (strip_color_mode pixels)
    size_t full_cap;
};

static uint32_t srm_bpp(ppa_srm_color_mode_t m) {
    return m == PPA_SRM_COLOR_MODE_RGB888 ? 3 : 2;  // RGB565 otherwise
}
static jpeg_dec_output_format_t srm_to_jpeg_fmt(ppa_srm_color_mode_t m) {
    return m == PPA_SRM_COLOR_MODE_RGB888 ? JPEG_DECODE_OUT_FORMAT_RGB888
                                          : JPEG_DECODE_OUT_FORMAT_RGB565;
}

esp_err_t jpeg_ppa_pipeline_new(const jpeg_ppa_pipeline_cfg_t *cfg,
                                jpeg_ppa_pipeline_handle_t *out_handle) {
    if (!cfg || !out_handle) return ESP_ERR_INVALID_ARG;
    struct jpeg_ppa_pipeline_s *h = calloc(1, sizeof(*h));
    if (!h) return ESP_ERR_NO_MEM;
    h->cfg = *cfg;

    jpeg_decode_engine_cfg_t ec = {0};
    esp_err_t err = jpeg_new_decoder_engine(&ec, &h->dec);
    if (err != ESP_OK) { free(h); return err; }

    ppa_client_config_t pc = {.oper_type = PPA_OPERATION_SRM};
    err = ppa_register_client(&pc, &h->ppa);
    if (err != ESP_OK) { jpeg_del_decoder_engine(h->dec); free(h); return err; }

    *out_handle = h;
    return ESP_OK;
}

esp_err_t jpeg_ppa_pipeline_del(jpeg_ppa_pipeline_handle_t h) {
    if (!h) return ESP_ERR_INVALID_ARG;
    if (h->ppa) ppa_unregister_client(h->ppa);
    if (h->dec) jpeg_del_decoder_engine(h->dec);
    free(h->full);
    free(h);
    return ESP_OK;
}

esp_err_t jpeg_ppa_pipeline_process(jpeg_ppa_pipeline_handle_t h,
                                    const void *jpeg_data, size_t jpeg_size,
                                    const jpeg_ppa_output_t *out,
                                    const jpeg_ppa_transform_t *transform,
                                    jpeg_enh_frame_info_t *info) {
    if (!h || !jpeg_data || !out || !out->buffer) return ESP_ERR_INVALID_ARG;
    jpeg_ppa_transform_t t = transform ? *transform : (jpeg_ppa_transform_t){0};

    jpeg_decode_picture_info_t pi;
    esp_err_t err = jpeg_decoder_get_info(jpeg_data, jpeg_size, &pi);
    if (err != ESP_OK) return err;
    if (pi.width > h->cfg.max_pic_w || pi.height > h->cfg.max_pic_h)
        return ESP_ERR_INVALID_SIZE;

    const uint32_t in_bpp = srm_bpp(h->cfg.strip_color_mode);
    size_t need = (size_t)pi.width * pi.height * in_bpp;
    if (need > h->full_cap) {
        free(h->full);
        h->full = malloc(need);
        h->full_cap = h->full ? need : 0;
        if (!h->full) return ESP_ERR_NO_MEM;
    }

    jpeg_decode_cfg_t dc = {
        .output_format = srm_to_jpeg_fmt(h->cfg.strip_color_mode),
        .rgb_order = h->cfg.rgb_order,
        .conv_std = h->cfg.conv_std,
    };
    uint32_t out_size = 0;
    err = jpeg_decoder_process(h->dec, &dc, jpeg_data, jpeg_size, h->full,
                               h->full_cap, &out_size);
    if (err != ESP_OK) return err;

    if (info) {
        memset(info, 0, sizeof(*info));
        info->origin_w = info->pic_w = pi.width;
        info->origin_h = info->pic_h = pi.height;
        info->mcu_w = info->mcu_h = 16;
        info->strip_h = pi.height;
        info->strip_count = 1;
    }

    // Crop: default (w==0/h==0) = whole image.
    uint32_t cx = t.in_crop.x, cy = t.in_crop.y;
    uint32_t cw = t.in_crop.w ? t.in_crop.w : pi.width;
    uint32_t ch = t.in_crop.h ? t.in_crop.h : pi.height;

    ppa_srm_oper_config_t op = {0};
    op.in.buffer = h->full;
    op.in.pic_w = pi.width;
    op.in.pic_h = pi.height;
    op.in.block_offset_x = cx;
    op.in.block_offset_y = cy;
    op.in.block_w = cw;
    op.in.block_h = ch;
    op.in.srm_cm = h->cfg.strip_color_mode;

    op.out.buffer = out->buffer;
    op.out.buffer_size =
        out->buffer_size ? out->buffer_size
                         : (size_t)out->pic_w * out->pic_h * srm_bpp(out->color_mode);
    op.out.pic_w = out->pic_w;
    op.out.pic_h = out->pic_h;
    op.out.block_offset_x = t.out_offset_x;
    op.out.block_offset_y = t.out_offset_y;
    op.out.srm_cm = out->color_mode;
    op.out.yuv_range = out->yuv_range;
    op.out.yuv_std = out->yuv_std;

    op.rotation_angle = t.rotation;
    op.scale_x = t.scale_x == 0.0f ? 1.0f : t.scale_x;
    op.scale_y = t.scale_y == 0.0f ? 1.0f : t.scale_y;
    op.mirror_x = t.mirror_x;
    op.mirror_y = t.mirror_y;
    op.rgb_swap = t.rgb_swap;
    op.byte_swap = t.byte_swap;
    op.mode = PPA_TRANS_MODE_BLOCKING;

    err = ppa_do_scale_rotate_mirror(h->ppa, &op);
    if (err != ESP_OK) ESP_LOGW(TAG, "ppa scale failed: %d", (int)err);
    return err;
}
