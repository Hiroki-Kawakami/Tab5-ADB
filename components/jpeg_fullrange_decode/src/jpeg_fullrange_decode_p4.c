/*
 * Full-range BT.601 variant of the IDF jpeg_decoder_process().
 *
 * This replays the IDF decode flow (header parse -> apply to HW -> single
 * full-frame 2D-DMA descriptor -> enqueue -> wait for RX EOF), but uses our own
 * 2D-DMA "job picked" callback so we can overwrite the CSC matrix registers
 * with the full-range (JFIF) BT.601 coefficients *after* the IDF helper has set
 * up the input/output muxing and scramble order, and *before* the transfer
 * starts.
 *
 * The engine itself is the stock IDF jpeg_decoder_t, created via
 * jpeg_new_decoder_engine(); we only reimplement the per-frame process step and
 * reach into the engine internals through the (component-private) IDF headers.
 */

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "jpeg_fullrange_decode.h"
#include "jpeg_private.h"
#include "private/jpeg_parse_marker.h"
#include "private/jpeg_param.h"
#include "esp_private/dma2d.h"
#include "hal/jpeg_ll.h"
#include "hal/jpeg_defs.h"
#include "hal/cache_ll.h"
#include "hal/cache_hal.h"
#include "hal/color_hal.h"
#include "hal/dma2d_ll.h"
#include "soc/dma2d_channel.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_cache.h"
#include "esp_dma_utils.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "jpeg.fullrange";

// =============================================================================
// Header parsing (replicas of the static helpers in IDF jpeg_decode.c)
// =============================================================================

static esp_err_t s_default_huff_table(jpeg_dec_header_info_t *header_info)
{
    memcpy(header_info->huffbits[0][0], luminance_dc_coefficients, JPEG_HUFFMAN_BITS_LEN_TABLE_LEN);
    memcpy(header_info->huffbits[0][1], chrominance_dc_coefficients, JPEG_HUFFMAN_BITS_LEN_TABLE_LEN);
    memcpy(header_info->huffbits[1][0], luminance_ac_coefficients, JPEG_HUFFMAN_BITS_LEN_TABLE_LEN);
    memcpy(header_info->huffbits[1][1], chrominance_ac_coefficients, JPEG_HUFFMAN_BITS_LEN_TABLE_LEN);
    memcpy(header_info->huffcode[0][0], luminance_dc_values, JPEG_HUFFMAN_DC_VALUE_TABLE_LEN);
    memcpy(header_info->huffcode[0][1], chrominance_dc_values, JPEG_HUFFMAN_DC_VALUE_TABLE_LEN);
    memcpy(header_info->huffcode[1][0], luminance_ac_values, JPEG_HUFFMAN_AC_VALUE_TABLE_LEN);
    memcpy(header_info->huffcode[1][1], chrominance_ac_values, JPEG_HUFFMAN_AC_VALUE_TABLE_LEN);
    return ESP_OK;
}

static esp_err_t s_parse_marker(jpeg_decoder_handle_t engine, const uint8_t *in_buf, uint32_t inbuf_len)
{
    jpeg_dec_header_info_t *header_info = engine->header_info;
    jpeg_hal_context_t *hal = &engine->codec_base->hal;

    memset(header_info, 0, sizeof(jpeg_dec_header_info_t));
    header_info->buffer_offset = (uint8_t *)in_buf;
    header_info->buffer_left = inbuf_len;
    engine->total_size = inbuf_len;
    header_info->header_size = 0;

    jpeg_ll_soft_rst(hal->dev);
    jpeg_ll_set_codec_mode(hal->dev, JPEG_CODEC_DECODER);
    // Digital issue: height/width must be cleared before decoding a new picture.
    jpeg_ll_set_picture_height(hal->dev, 0);
    jpeg_ll_set_picture_width(hal->dev, 0);

    while (header_info->buffer_left) {
        uint8_t lastchar = jpeg_get_bytes(header_info, 1);
        uint8_t thischar = jpeg_get_bytes(header_info, 1);
        uint16_t marker = (lastchar << 8 | thischar);
        switch (marker) {
        case JPEG_M_SOI:
            break;
        case JPEG_M_APP0: case JPEG_M_APP1: case JPEG_M_APP2: case JPEG_M_APP3:
        case JPEG_M_APP4: case JPEG_M_APP5: case JPEG_M_APP6: case JPEG_M_APP7:
        case JPEG_M_APP8: case JPEG_M_APP9: case JPEG_M_APP10: case JPEG_M_APP11:
        case JPEG_M_APP12: case JPEG_M_APP13: case JPEG_M_APP14: case JPEG_M_APP15:
            ESP_RETURN_ON_ERROR(jpeg_parse_appn_marker(header_info), TAG, "deal appn marker failed");
            break;
        case JPEG_M_COM:
            ESP_RETURN_ON_ERROR(jpeg_parse_com_marker(header_info), TAG, "deal com marker failed");
            break;
        case JPEG_M_DQT:
            ESP_RETURN_ON_ERROR(jpeg_parse_dqt_marker(header_info), TAG, "deal dqt marker failed");
            break;
        case JPEG_M_SOF0:
            ESP_RETURN_ON_ERROR(jpeg_parse_sof_marker(header_info), TAG, "deal sof marker failed");
            break;
        case JPEG_M_SOF1: case JPEG_M_SOF2: case JPEG_M_SOF3: case JPEG_M_SOF5:
        case JPEG_M_SOF6: case JPEG_M_SOF7: case JPEG_M_SOF9: case JPEG_M_SOF10:
        case JPEG_M_SOF11: case JPEG_M_SOF13: case JPEG_M_SOF14: case JPEG_M_SOF15:
            ESP_LOGE(TAG, "Only baseline-DCT is supported.");
            return ESP_ERR_NOT_SUPPORTED;
        case JPEG_M_DRI:
            ESP_RETURN_ON_ERROR(jpeg_parse_dri_marker(header_info), TAG, "deal dri marker failed");
            break;
        case JPEG_M_DHT:
            ESP_RETURN_ON_ERROR(jpeg_parse_dht_marker(header_info), TAG, "deal dht marker failed");
            break;
        case JPEG_M_SOS:
            ESP_RETURN_ON_ERROR(jpeg_parse_sos_marker(header_info), TAG, "deal sos marker failed");
            break;
        case JPEG_M_INV:
            ESP_RETURN_ON_ERROR(jpeg_parse_inv_marker(header_info), TAG, "deal invalid marker failed");
            break;
        }
        if (marker == JPEG_M_SOS) {
            break;
        }
    }

    header_info->buffer_left = engine->total_size - header_info->header_size;

    if (!header_info->dht_marker) {
        // USB cameras commonly omit the Huffman table to save bus bandwidth.
        s_default_huff_table(header_info);
    }
    return ESP_OK;
}

static esp_err_t s_color_space_support_check(jpeg_decoder_handle_t engine)
{
    if (engine->sample_method == JPEG_DOWN_SAMPLING_YUV444) {
        if (engine->output_format == JPEG_DECODE_OUT_FORMAT_YUV422 || engine->output_format == JPEG_DECODE_OUT_FORMAT_YUV420) {
            ESP_LOGE(TAG, "Detected YUV444 but want to convert to YUV422/YUV420, which is not supported");
            return ESP_ERR_INVALID_ARG;
        }
    } else if (engine->sample_method == JPEG_DOWN_SAMPLING_YUV422) {
        if (engine->output_format == JPEG_DECODE_OUT_FORMAT_YUV420) {
            ESP_LOGE(TAG, "Detected YUV422 but want to convert to YUV420, which is not supported");
            return ESP_ERR_INVALID_ARG;
        }
    } else if (engine->sample_method == JPEG_DOWN_SAMPLING_YUV420) {
        if (engine->output_format == JPEG_DECODE_OUT_FORMAT_YUV422) {
            ESP_LOGE(TAG, "Detected YUV420 but want to convert to YUV422, which is not supported");
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static esp_err_t s_apply_header_to_hw(jpeg_decoder_handle_t engine)
{
    jpeg_dec_header_info_t *header_info = engine->header_info;
    jpeg_hal_context_t *hal = &engine->codec_base->hal;

    for (int i = 0; i < header_info->qt_tbl_num; i++) {
        dqt_func[i](hal->dev, header_info->qt_tbl[i]);
    }
    jpeg_ll_set_picture_height(hal->dev, header_info->process_v);
    jpeg_ll_set_picture_width(hal->dev, header_info->process_h);
    jpeg_ll_set_decode_component_num(hal->dev, header_info->nf);
    for (int i = 0; i < header_info->nf; i++) {
        sof_func[i](hal->dev, header_info->ci[i], header_info->hi[i], header_info->vi[i], header_info->qtid[i]);
    }
    if (header_info->nf == 3) {
        switch (header_info->hivi[0]) {
        case 0x11: engine->sample_method = JPEG_DOWN_SAMPLING_YUV444; break;
        case 0x21: engine->sample_method = JPEG_DOWN_SAMPLING_YUV422; break;
        case 0x22: engine->sample_method = JPEG_DOWN_SAMPLING_YUV420; break;
        default:
            ESP_LOGE(TAG, "Sampling factor cannot be recognized");
            return ESP_ERR_INVALID_STATE;
        }
    } else if (header_info->nf == 1) {
        if (engine->output_format != JPEG_DECODE_OUT_FORMAT_GRAY) {
            ESP_LOGE(TAG, "your jpg is a gray style picture, but your output format is wrong");
            return ESP_ERR_NOT_SUPPORTED;
        }
        engine->sample_method = JPEG_DOWN_SAMPLING_GRAY;
    }

    ESP_RETURN_ON_ERROR(s_color_space_support_check(engine), TAG, "unsupported output/sampling combination");

    // Recomputed every frame (the engine is reused across decodes).
    engine->no_color_conversion = ((uint32_t)engine->sample_method == (uint32_t)engine->output_format);

    dht_func[0][0](hal, header_info->huffbits[0][0], header_info->huffcode[0][0], header_info->tmp_huff);
    dht_func[0][1](hal, header_info->huffbits[0][1], header_info->huffcode[0][1], header_info->tmp_huff);
    dht_func[1][0](hal, header_info->huffbits[1][0], header_info->huffcode[1][0], header_info->tmp_huff);
    dht_func[1][1](hal, header_info->huffbits[1][1], header_info->huffcode[1][1], header_info->tmp_huff);

    jpeg_ll_set_restart_interval(hal->dev, header_info->ri);
    return ESP_OK;
}

// =============================================================================
// 2D-DMA descriptor configuration (single full-frame, like IDF)
// =============================================================================

static void s_cfg_desc(jpeg_decoder_handle_t engine, dma2d_descriptor_t *dsc,
                       uint8_t en_2d, uint8_t mode, uint16_t vb, uint16_t hb,
                       uint8_t eof, uint32_t pbyte, uint8_t owner,
                       uint16_t va, uint16_t ha, uint8_t *buf, dma2d_descriptor_t *next_dsc)
{
    dsc->dma2d_en  = en_2d;
    dsc->mode      = mode;
    dsc->vb_size   = vb;
    dsc->hb_length = hb;
    dsc->pbyte     = pbyte;
    dsc->suc_eof   = eof;
    dsc->owner     = owner;
    dsc->va_size   = va;
    dsc->ha_length = ha;
    dsc->buffer    = buf;
    dsc->next      = next_dsc;
    esp_err_t ret = esp_cache_msync((void *)dsc, engine->dma_desc_size,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    assert(ret == ESP_OK);
}

static esp_err_t s_config_dma_descriptor(jpeg_decoder_handle_t engine)
{
    jpeg_dec_format_hb_t best_hb_idx = 0;
    color_space_pixel_format_t picture_format = { .color_type_id = engine->output_format };
    engine->bit_per_pixel = color_hal_pixel_format_get_bit_depth(picture_format);

    if (!engine->no_color_conversion) {
        switch (engine->output_format) {
        case JPEG_DECODE_OUT_FORMAT_RGB888: best_hb_idx = JPEG_DEC_RGB888_HB; break;
        case JPEG_DECODE_OUT_FORMAT_RGB565: best_hb_idx = JPEG_DEC_RGB565_HB; break;
        case JPEG_DECODE_OUT_FORMAT_GRAY:   best_hb_idx = JPEG_DEC_GRAY_HB;   break;
        case JPEG_DECODE_OUT_FORMAT_YUV444: best_hb_idx = JPEG_DEC_YUV444_HB; break;
        default:
            ESP_LOGE(TAG, "unsupported output format");
            return ESP_ERR_NOT_SUPPORTED;
        }
    } else {
        best_hb_idx = JPEG_DEC_DIRECT_OUTPUT_HB;
    }

    uint8_t sample_method_idx;
    switch (engine->sample_method) {
    case JPEG_DOWN_SAMPLING_YUV444: sample_method_idx = 0; break;
    case JPEG_DOWN_SAMPLING_YUV422: sample_method_idx = 1; break;
    case JPEG_DOWN_SAMPLING_YUV420: sample_method_idx = 2; break;
    case JPEG_DOWN_SAMPLING_GRAY:   sample_method_idx = 3; break;
    default:
        ESP_LOGE(TAG, "unsupported sampling mode");
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t dma_hb = dec_hb_tbl[sample_method_idx][best_hb_idx];
    uint32_t dma_vb = engine->header_info->mcuy;

    s_cfg_desc(engine, engine->txlink, JPEG_DMA2D_2D_DISABLE, DMA2D_DESCRIPTOR_BLOCK_RW_MODE_SINGLE,
               engine->header_info->buffer_left & JPEG_DMA2D_MAX_SIZE,
               engine->header_info->buffer_left & JPEG_DMA2D_MAX_SIZE,
               JPEG_DMA2D_EOF_NOT_LAST, 1, DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA,
               (engine->header_info->buffer_left >> JPEG_DMA2D_1D_HIGH_14BIT),
               (engine->header_info->buffer_left >> JPEG_DMA2D_1D_HIGH_14BIT),
               engine->header_info->buffer_offset, NULL);

    s_cfg_desc(engine, engine->rxlink, JPEG_DMA2D_2D_ENABLE, DMA2D_DESCRIPTOR_BLOCK_RW_MODE_MULTIPLE,
               dma_vb, dma_hb, JPEG_DMA2D_EOF_NOT_LAST,
               dma2d_desc_pixel_format_to_pbyte_value(picture_format),
               DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA,
               engine->header_info->process_v, engine->header_info->process_h,
               engine->decoded_buf, NULL);

    return ESP_OK;
}

// =============================================================================
// 2D-DMA channel & CSC plumbing
// =============================================================================

static bool s_rx_eof(dma2d_channel_handle_t chan, dma2d_event_data_t *evt, void *user_data)
{
    (void)chan; (void)evt;
    portBASE_TYPE hp = pdFALSE;
    jpeg_decoder_handle_t engine = (jpeg_decoder_handle_t)user_data;
    jpeg_dma2d_dec_evt_t dec_evt = { .jpgd_status = 0, .dma_evt = JPEG_DMA2D_RX_EOF };
    xQueueSendFromISR(engine->evt_queue, &dec_evt, &hp);
    return hp == pdTRUE;
}

static void s_config_trans_ability(jpeg_decoder_handle_t engine)
{
    dma2d_transfer_ability_t tx_ab = {
        .data_burst_length = DMA2D_DATA_BURST_LENGTH_128,
        .desc_burst_en = true,
        .mb_size = DMA2D_MACRO_BLOCK_SIZE_NONE,
    };
    dma2d_transfer_ability_t rx_ab = tx_ab;
    switch (engine->sample_method) {
    case JPEG_DOWN_SAMPLING_YUV444: rx_ab.mb_size = DMA2D_MACRO_BLOCK_SIZE_8_8;   break;
    case JPEG_DOWN_SAMPLING_YUV422: rx_ab.mb_size = DMA2D_MACRO_BLOCK_SIZE_8_16;  break;
    case JPEG_DOWN_SAMPLING_YUV420: rx_ab.mb_size = DMA2D_MACRO_BLOCK_SIZE_16_16; break;
    case JPEG_DOWN_SAMPLING_GRAY:   rx_ab.mb_size = DMA2D_MACRO_BLOCK_SIZE_8_8;   break;
    default: break;
    }
    dma2d_set_transfer_ability(engine->dma2d_tx_channel, &tx_ab);
    dma2d_set_transfer_ability(engine->dma2d_rx_channel, &rx_ab);
}

// Full-range BT.601 YCbCr->RGB matrix (JPEG/JFIF convention, Y/Cb/Cr all in
// [0,255], output in [0,255]):
//     R = Y                          + 1.402  *(Cr - 128)
//     G = Y - 0.344136*(Cb - 128) - 0.714136*(Cr - 128)
//     B = Y + 1.772  *(Cb - 128)
//
// The 2D-DMA CSC unit evaluates  256 * Q = A*Y + B*Cb + C*Cr + D  with signed
// fields A[9:0]/B[10:0]/C[9:0]/D[17:0] (see in_color_param_h/m/l_ch0 in the
// ESP32-P4 TRM). The IDF default (DMA2D_COLOR_SPACE_CONV_PARAM_YUV2RGB_BT601)
// bakes in the limited-range (Y_studio in [16,235]) 1.164*(Y-16) form, which
// under-saturates full-range MJPEG output. We let the IDF helper set up the
// input/output muxing + scramble, then overwrite the matrix here.
//
// Coefficients * 256 (rounded): 1.0->256, 1.402->359, 0.344136->88,
// 0.714136->183, 1.772->454. Offsets put a neutral input (Y=0, Cb=128, Cr=128)
// at RGB=0: D_R = -359*128, D_G = (88+183)*128, D_B = -454*128.
static const int s_yuv2rgb_bt601_full_table[3][4] = {
    { 256,    0,   359,  -45952 },  // R: param_h
    { 256,  -88,  -183,   34688 },  // G: param_m
    { 256,  454,     0,  -58112 },  // B: param_l
};

static void s_load_full_range_bt601_matrix(void)
{
    dma2d_dev_t *dev = DMA2D_LL_GET_HW(0);
    // Only RX channel 0 implements CSC (DMA2D_LL_RX_CHANNEL_SUPPORT_CSC_MASK = BIT0).
    volatile dma2d_color_param_group_chn_reg_t *grp = &dev->in_channel0.in_color_param_group;
    grp->param_h.a = s_yuv2rgb_bt601_full_table[0][0];
    grp->param_h.b = s_yuv2rgb_bt601_full_table[0][1];
    grp->param_h.c = s_yuv2rgb_bt601_full_table[0][2];
    grp->param_h.d = s_yuv2rgb_bt601_full_table[0][3];
    grp->param_m.a = s_yuv2rgb_bt601_full_table[1][0];
    grp->param_m.b = s_yuv2rgb_bt601_full_table[1][1];
    grp->param_m.c = s_yuv2rgb_bt601_full_table[1][2];
    grp->param_m.d = s_yuv2rgb_bt601_full_table[1][3];
    grp->param_l.a = s_yuv2rgb_bt601_full_table[2][0];
    grp->param_l.b = s_yuv2rgb_bt601_full_table[2][1];
    grp->param_l.c = s_yuv2rgb_bt601_full_table[2][2];
    grp->param_l.d = s_yuv2rgb_bt601_full_table[2][3];
}

static void s_config_csc(jpeg_decoder_handle_t engine, dma2d_channel_handle_t rx)
{
    dma2d_scramble_order_t post = DMA2D_SCRAMBLE_ORDER_BYTE2_1_0;
    dma2d_csc_rx_option_t opt = DMA2D_CSC_RX_NONE;
    bool yuv_to_rgb_bt601 = false;

    if (engine->rgb_order == JPEG_DEC_RGB_ELEMENT_ORDER_RGB) {
        if (engine->output_format == JPEG_DECODE_OUT_FORMAT_RGB565) post = DMA2D_SCRAMBLE_ORDER_BYTE2_0_1;
        else if (engine->output_format == JPEG_DECODE_OUT_FORMAT_RGB888) post = DMA2D_SCRAMBLE_ORDER_BYTE0_1_2;
    }
    if (engine->output_format == JPEG_DECODE_OUT_FORMAT_RGB565) {
        opt = (engine->conv_std == JPEG_YUV_RGB_CONV_STD_BT601)
              ? DMA2D_CSC_RX_YUV420_TO_RGB565_601 : DMA2D_CSC_RX_YUV420_TO_RGB565_709;
        yuv_to_rgb_bt601 = (engine->conv_std == JPEG_YUV_RGB_CONV_STD_BT601);
    } else if (engine->output_format == JPEG_DECODE_OUT_FORMAT_RGB888) {
        opt = (engine->conv_std == JPEG_YUV_RGB_CONV_STD_BT601)
              ? DMA2D_CSC_RX_YUV420_TO_RGB888_601 : DMA2D_CSC_RX_YUV420_TO_RGB888_709;
        yuv_to_rgb_bt601 = (engine->conv_std == JPEG_YUV_RGB_CONV_STD_BT601);
    } else if (engine->output_format == JPEG_DECODE_OUT_FORMAT_YUV444) {
        if (engine->sample_method == JPEG_DOWN_SAMPLING_YUV422)      opt = DMA2D_CSC_RX_YUV422_TO_YUV444;
        else if (engine->sample_method == JPEG_DOWN_SAMPLING_YUV420) opt = DMA2D_CSC_RX_YUV420_TO_YUV444;
    }

    dma2d_csc_config_t cfg = { .post_scramble = post, .rx_csc_option = opt };
    dma2d_configure_color_space_conversion(rx, &cfg);
    // IDF just wrote the limited-range BT.601 matrix; clobber it with the
    // full-range (JFIF) form that matches MJPEG content.
    if (yuv_to_rgb_bt601) {
        s_load_full_range_bt601_matrix();
    }
}

static bool s_on_job_picked(uint32_t channel_num, const dma2d_trans_channel_info_t *chans, void *user_config)
{
    assert(channel_num == 2);
    jpeg_decoder_handle_t engine = (jpeg_decoder_handle_t)user_config;
    jpeg_hal_context_t *hal = &engine->codec_base->hal;

    dma2d_channel_handle_t tx = NULL, rx = NULL;
    for (uint32_t i = 0; i < channel_num; i++) {
        if (chans[i].dir == DMA2D_CHANNEL_DIRECTION_TX) tx = chans[i].chan;
        else rx = chans[i].chan;
    }
    engine->dma2d_tx_channel = tx;
    engine->dma2d_rx_channel = rx;

    dma2d_trigger_t trig = {
        .periph = DMA2D_TRIG_PERIPH_JPEG_DECODER,
        .periph_sel_id = SOC_DMA2D_TRIG_PERIPH_JPEG_TX,
    };
    dma2d_connect(tx, &trig);
    trig.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_JPEG_RX;
    dma2d_connect(rx, &trig);

    s_config_trans_ability(engine);
    s_config_csc(engine, rx);

    dma2d_rx_event_callbacks_t cbs = { .on_recv_eof = s_rx_eof };
    dma2d_register_rx_event_callbacks(rx, &cbs, engine);

    dma2d_set_desc_addr(tx, (intptr_t)engine->txlink);
    dma2d_set_desc_addr(rx, (intptr_t)engine->rxlink);
    dma2d_start(tx);
    dma2d_start(rx);
    jpeg_ll_process_start(hal->dev);
    return false;
}

// =============================================================================
// Public API
// =============================================================================

esp_err_t jpeg_decoder_process_full_range(
    jpeg_decoder_handle_t decoder_engine,
    const jpeg_decode_cfg_t *decode_cfg,
    const uint8_t *bit_stream, uint32_t stream_size,
    uint8_t *decode_outbuf, uint32_t outbuf_size,
    uint32_t *out_size)
{
    ESP_RETURN_ON_FALSE(decoder_engine, ESP_ERR_INVALID_ARG, TAG, "jpeg decode handle is null");
    ESP_RETURN_ON_FALSE(decode_cfg, ESP_ERR_INVALID_ARG, TAG, "jpeg decode config is null");
    ESP_RETURN_ON_FALSE(decode_outbuf, ESP_ERR_INVALID_ARG, TAG, "jpeg decode picture buffer is null");
    ESP_RETURN_ON_FALSE(out_size, ESP_ERR_INVALID_ARG, TAG, "out_size is null");

    esp_dma_mem_info_t dma_mem_info = { .dma_alignment_bytes = 4 };
    ESP_RETURN_ON_FALSE(esp_dma_is_buffer_alignment_satisfied(decode_outbuf, outbuf_size, dma_mem_info),
                        ESP_ERR_INVALID_ARG, TAG,
                        "output buffer/size not aligned, use jpeg_alloc_decoder_mem");

    esp_err_t ret = ESP_OK;
    bool need_yield;

    if (decoder_engine->codec_base->pm_lock) {
        ESP_RETURN_ON_ERROR(esp_pm_lock_acquire(decoder_engine->codec_base->pm_lock), TAG, "acquire pm_lock failed");
    }
    xSemaphoreTake(decoder_engine->codec_base->codec_mutex, portMAX_DELAY);
    xQueueReset(decoder_engine->evt_queue);

    decoder_engine->output_format = decode_cfg->output_format;
    decoder_engine->rgb_order = decode_cfg->rgb_order;
    decoder_engine->conv_std = decode_cfg->conv_std;
    decoder_engine->decoded_buf = decode_outbuf;

    ESP_GOTO_ON_ERROR(s_parse_marker(decoder_engine, bit_stream, stream_size), err2, TAG, "jpeg parse marker failed");
    ESP_GOTO_ON_ERROR(s_apply_header_to_hw(decoder_engine), err2, TAG, "write header info to hw failed");
    ESP_GOTO_ON_ERROR(s_config_dma_descriptor(decoder_engine), err2, TAG, "config dma descriptor failed");

    *out_size = decoder_engine->header_info->process_h * decoder_engine->header_info->process_v * decoder_engine->bit_per_pixel / 8;
    ESP_GOTO_ON_FALSE((*out_size <= outbuf_size), ESP_ERR_INVALID_ARG, err2, TAG,
                      "output buffer %" PRIu32 " smaller than decode size %" PRIu32, outbuf_size, *out_size);

    dma2d_trans_config_t trans = {
        .tx_channel_num = 1,
        .rx_channel_num = 1,
        .channel_flags = DMA2D_CHANNEL_FUNCTION_FLAG_RX_REORDER,
        .user_config = decoder_engine,
        .on_job_picked = s_on_job_picked,
    };

    // Sync input from cache to PSRAM so DMA reads the right bytes; invalidate
    // the output region ahead of the DMA write.
    ret = esp_cache_msync((void *)decoder_engine->header_info->buffer_offset,
                          decoder_engine->header_info->buffer_left,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    assert(ret == ESP_OK);
    ret = esp_cache_msync((void *)decoder_engine->decoded_buf, outbuf_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    assert(ret == ESP_OK);

    ESP_GOTO_ON_ERROR(dma2d_enqueue(decoder_engine->dma2d_group_handle, &trans, decoder_engine->trans_desc),
                      err2, TAG, "enqueue dma2d failed");

    while (1) {
        jpeg_dma2d_dec_evt_t evt;
        BaseType_t r = xQueueReceive(decoder_engine->evt_queue, &evt, decoder_engine->timeout_tick);
        ESP_GOTO_ON_FALSE(r == pdTRUE, ESP_ERR_TIMEOUT, err1, TAG, "jpeg decode timeout");

        if (evt.jpgd_status != 0) {
            ESP_LOGE(TAG, "jpeg decode error status=0x%" PRIx32, (uint32_t)evt.jpgd_status);
            ret = ESP_ERR_INVALID_STATE;
            goto err1;
        }
        if (evt.dma_evt & JPEG_DMA2D_RX_EOF) {
            ret = esp_cache_msync((void *)decoder_engine->decoded_buf, outbuf_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            assert(ret == ESP_OK);
            break;
        }
    }

    xSemaphoreGive(decoder_engine->codec_base->codec_mutex);
    if (decoder_engine->codec_base->pm_lock) {
        ESP_RETURN_ON_ERROR(esp_pm_lock_release(decoder_engine->codec_base->pm_lock), TAG, "release pm_lock failed");
    }
    return ESP_OK;

err1:
    dma2d_force_end(decoder_engine->trans_desc, &need_yield);
err2:
    xSemaphoreGive(decoder_engine->codec_base->codec_mutex);
    if (decoder_engine->codec_base->pm_lock) {
        esp_pm_lock_release(decoder_engine->codec_base->pm_lock);
    }
    return ret;
}
