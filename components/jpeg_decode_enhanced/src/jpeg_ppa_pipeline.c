/*
 * jpeg_decode_enhanced — Layer 2 implementation.
 *
 * Each jpeg_ppa_pipeline_process() call pushes a single JPEG frame through:
 *   1) strip-decode into the ring buffers (Layer 1, in the calling thread)
 *   2) per-strip PPA SRM into the destination frame buffer (worker task)
 *
 * The strip decoder fires on_strip_done from ISR; each strip event is
 * forwarded to a dedicated PPA worker task via a queue. The worker calls
 * ppa_do_scale_rotate_mirror() in BLOCKING mode (which only blocks the
 * worker, not the decode thread) and then releases the strip so the chained
 * DMA can recycle its ring slot for a later strip of the same image.
 *
 * Wait-for-frame: the decode call returns once the JPEG hardware reports
 * RX_EOF, but the last few PPA strips may still be in flight; process()
 * blocks on all_done_sem (released by the worker after the final strip)
 * before returning.
 *
 * Strip placement: the per-frame transform is resolved in the on_frame_start
 * callback (decode-thread context, before any strip arrives) into integer
 * "sixteenths" scale factors matching PPA's 1/16 quantization. Each strip's
 * output rect is then derived by mapping its input row band through
 * scale -> rotate -> mirror over the cropped image extent, so all rotations,
 * mirrors and crops share one placement computation.
 */

#include <string.h>
#include "jpeg_ppa_pipeline.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hal/color_hal.h"

static const char *TAG = "jpeg_ppa";

#define DEFAULT_WORKER_STACK  4096
#define DEFAULT_WORKER_PRIO   17
#define SCALE_FRAG_MAX        16        // PPA scale quantization: 1/16 steps
#define ABORT_DRAIN_TIMEOUT   pdMS_TO_TICKS(1000)

// Poison strip_idx that tells the worker task to exit.
#define WORKER_EXIT_MARKER    UINT32_MAX

struct jpeg_ppa_pipeline_s {
    jpeg_ppa_pipeline_cfg_t cfg;
    jpeg_enh_strip_decoder_handle_t decoder;
    ppa_client_handle_t ppa_client;

    QueueHandle_t worker_queue;          // jpeg_enh_strip_event_t items
    SemaphoreHandle_t all_done;
    SemaphoreHandle_t worker_joined;
    TaskHandle_t worker_task;

    // Per-frame state, written by process()/on_frame_start before any strip
    // event can reach the worker, read-only afterwards.
    struct {
        jpeg_enh_frame_info_t frame;
        jpeg_ppa_output_t out;           // buffer_size resolved
        jpeg_ppa_transform_t t;          // scales normalized (never 0)
        jpeg_ppa_rect_t crop;            // resolved input crop (within origin)
        uint32_t fx, fy;                 // quantized scales in 1/16 units
        uint32_t scaled_w;               // crop.w scaled by fx (output px)
        uint32_t scaled_h;               // crop.h scaled by fy (output px)
        uint32_t ext_w, ext_h;           // scaled crop extent after rotation
        bool valid;                      // on_frame_start completed
    } cur;
    volatile uint32_t pending_strips;
    volatile esp_err_t frame_err;
    volatile bool abort_frame;
};

static inline uint32_t s_scaled(uint32_t v, uint32_t f16) { return (uint32_t)(((uint64_t)v * f16) / SCALE_FRAG_MAX); }

static uint32_t s_quantize_scale(float s, bool yuv420_out)
{
    uint32_t i = (uint32_t)s;
    uint32_t frag = (uint32_t)(s * SCALE_FRAG_MAX) & (SCALE_FRAG_MAX - 1);
    if (yuv420_out) frag &= ~1u;  // PPA forces even frag for YUV420 output
    return i * SCALE_FRAG_MAX + frag;
}

static jpeg_dec_output_format_t s_jpeg_out_for_strip_cm(ppa_srm_color_mode_t cm)
{
    switch (cm) {
    case PPA_SRM_COLOR_MODE_RGB565: return JPEG_DECODE_OUT_FORMAT_RGB565;
    case PPA_SRM_COLOR_MODE_RGB888: return JPEG_DECODE_OUT_FORMAT_RGB888;
    case PPA_SRM_COLOR_MODE_YUV420: return JPEG_DECODE_OUT_FORMAT_YUV420;
    case PPA_SRM_COLOR_MODE_YUV444: return JPEG_DECODE_OUT_FORMAT_YUV444;
    default:                        return (jpeg_dec_output_format_t)-1;
    }
}

static inline bool s_cm_is_yuv(ppa_srm_color_mode_t cm)
{
    return cm == PPA_SRM_COLOR_MODE_YUV420 || cm == PPA_SRM_COLOR_MODE_YUV444;
}

static inline uint32_t s_cm_bits(ppa_srm_color_mode_t cm)
{
    color_space_pixel_format_t f = { .color_type_id = cm };
    return color_hal_pixel_format_get_bit_depth(f);
}

// -----------------------------------------------------------------------------
// Per-frame transform resolution (decode-thread context, before strips flow)
// -----------------------------------------------------------------------------

static esp_err_t s_on_frame_start(const jpeg_enh_frame_info_t *info, void *user_ctx)
{
    jpeg_ppa_pipeline_handle_t h = (jpeg_ppa_pipeline_handle_t)user_ctx;
    h->cur.frame = *info;

    // Resolve the input crop against the valid (non-padded) image area.
    jpeg_ppa_rect_t c = h->cur.t.in_crop;
    if (c.w == 0 || c.h == 0) {
        c = (jpeg_ppa_rect_t){ 0, 0, info->origin_w, info->origin_h };
    }
    if (c.x >= info->origin_w || c.y >= info->origin_h) {
        ESP_LOGE(TAG, "in_crop origin (%lu,%lu) outside image %lux%lu",
                 (unsigned long)c.x, (unsigned long)c.y,
                 (unsigned long)info->origin_w, (unsigned long)info->origin_h);
        return ESP_ERR_INVALID_ARG;
    }
    if (c.x + c.w > info->origin_w) c.w = info->origin_w - c.x;
    if (c.y + c.h > info->origin_h) c.h = info->origin_h - c.y;
    h->cur.crop = c;

    bool yuv420_out = (h->cur.out.color_mode == PPA_SRM_COLOR_MODE_YUV420);
    h->cur.fx = s_quantize_scale(h->cur.t.scale_x, yuv420_out);
    h->cur.fy = s_quantize_scale(h->cur.t.scale_y, yuv420_out);
    if (h->cur.fx == 0 || h->cur.fy == 0) {
        ESP_LOGE(TAG, "scale below PPA minimum (1/16)");
        return ESP_ERR_INVALID_ARG;
    }

    // Strips tile the output along the scaled y axis, so every interior strip
    // boundary must land on a whole output row once scaled. (Always true for
    // strip heights that are multiples of 16.)
    for (uint32_t k = c.y / info->strip_h + 1; (uint64_t)k * info->strip_h < c.y + c.h; k++) {
        uint32_t rel = k * info->strip_h - c.y;
        if ((rel * h->cur.fy) % SCALE_FRAG_MAX != 0) {
            ESP_LOGE(TAG, "strip boundary %lu x scale %lu/16 is not a whole output row; "
                          "adjust scale_y, in_crop.y or strip_h_hint",
                     (unsigned long)rel, (unsigned long)h->cur.fy);
            return ESP_ERR_INVALID_ARG;
        }
    }

    h->cur.scaled_w = s_scaled(c.w, h->cur.fx);
    h->cur.scaled_h = s_scaled(c.h, h->cur.fy);
    bool swap = (h->cur.t.rotation == PPA_SRM_ROTATION_ANGLE_90 ||
                 h->cur.t.rotation == PPA_SRM_ROTATION_ANGLE_270);
    h->cur.ext_w = swap ? h->cur.scaled_h : h->cur.scaled_w;
    h->cur.ext_h = swap ? h->cur.scaled_w : h->cur.scaled_h;

    if (h->cur.t.out_offset_x + h->cur.ext_w > h->cur.out.pic_w ||
        h->cur.t.out_offset_y + h->cur.ext_h > h->cur.out.pic_h) {
        ESP_LOGE(TAG, "rendered rect %lux%lu at (%lu,%lu) exceeds output %lux%lu",
                 (unsigned long)h->cur.ext_w, (unsigned long)h->cur.ext_h,
                 (unsigned long)h->cur.t.out_offset_x, (unsigned long)h->cur.t.out_offset_y,
                 (unsigned long)h->cur.out.pic_w, (unsigned long)h->cur.out.pic_h);
        return ESP_ERR_INVALID_ARG;
    }

    h->cur.valid = true;
    h->pending_strips = info->strip_count;
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Strip handoff (ISR) and PPA worker
// -----------------------------------------------------------------------------

static bool s_on_strip_done_isr(const jpeg_enh_strip_event_t *evt, void *user_ctx)
{
    jpeg_ppa_pipeline_handle_t h = (jpeg_ppa_pipeline_handle_t)user_ctx;
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(h->worker_queue, evt, &hp);
    return hp == pdTRUE;
}

// Map strip rows [a_rel, b_rel) (crop-relative) through scale+rotation+mirror
// into the output rect for this strip's PPA op.
static void s_strip_out_rect(jpeg_ppa_pipeline_handle_t h, uint32_t a_rel, uint32_t b_rel,
                             uint32_t *out_x, uint32_t *out_y)
{
    uint32_t A = s_scaled(a_rel, h->cur.fy);
    uint32_t B = s_scaled(b_rel, h->cur.fy);
    uint32_t band = B - A;
    uint32_t x, y, w, hgt;

    // Placement convention matches PPA hardware behaviour as observed on
    // ESP32-P4 (strip 0 = image top): ANGLE_90 puts the top input band at
    // output x=0, ANGLE_270 at the right edge.
    switch (h->cur.t.rotation) {
    default:
    case PPA_SRM_ROTATION_ANGLE_0:
        x = 0;                       y = A;                       w = h->cur.scaled_w; hgt = band;
        break;
    case PPA_SRM_ROTATION_ANGLE_90:
        x = A;                       y = 0;                       w = band;            hgt = h->cur.scaled_w;
        break;
    case PPA_SRM_ROTATION_ANGLE_180:
        x = 0;                       y = h->cur.scaled_h - B;     w = h->cur.scaled_w; hgt = band;
        break;
    case PPA_SRM_ROTATION_ANGLE_270:
        x = h->cur.scaled_h - B;     y = 0;                       w = band;            hgt = h->cur.scaled_w;
        break;
    }

    // PPA mirrors the pixels inside each block; flipping the block placement
    // across the full extent completes the global mirror.
    if (h->cur.t.mirror_x) x = h->cur.ext_w - x - w;
    if (h->cur.t.mirror_y) y = h->cur.ext_h - y - hgt;

    *out_x = h->cur.t.out_offset_x + x;
    *out_y = h->cur.t.out_offset_y + y;
}

static esp_err_t s_process_strip(jpeg_ppa_pipeline_handle_t h, const jpeg_enh_strip_event_t *evt)
{
    const jpeg_ppa_rect_t *c = &h->cur.crop;

    // Intersect this strip's valid rows with the crop band.
    uint32_t a_abs = c->y > evt->y_offset ? c->y : evt->y_offset;
    uint32_t strip_end = evt->y_offset + evt->rows;
    uint32_t crop_end = c->y + c->h;
    uint32_t b_abs = strip_end < crop_end ? strip_end : crop_end;
    if (b_abs <= a_abs) return ESP_OK;  // strip entirely outside the crop

    ppa_srm_oper_config_t op = { 0 };
    op.in.buffer         = evt->buffer;
    op.in.pic_w          = h->cur.frame.pic_w;
    op.in.pic_h          = evt->padded_rows;
    op.in.block_offset_x = c->x;
    op.in.block_w        = c->w;
    op.in.block_offset_y = a_abs - evt->y_offset;
    op.in.block_h        = b_abs - a_abs;
    op.in.srm_cm         = h->cfg.strip_color_mode;
    if (s_cm_is_yuv(h->cfg.strip_color_mode)) {
        op.in.yuv_range = h->cfg.yuv_full_range ? PPA_COLOR_RANGE_FULL : PPA_COLOR_RANGE_LIMIT;
        op.in.yuv_std   = (h->cfg.conv_std == JPEG_YUV_RGB_CONV_STD_BT709)
                          ? PPA_COLOR_CONV_STD_RGB_YUV_BT709 : PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    }

    op.out.buffer      = h->cur.out.buffer;
    op.out.buffer_size = h->cur.out.buffer_size;
    op.out.pic_w       = h->cur.out.pic_w;
    op.out.pic_h       = h->cur.out.pic_h;
    op.out.srm_cm      = h->cur.out.color_mode;
    op.out.yuv_range   = h->cur.out.yuv_range;
    op.out.yuv_std     = h->cur.out.yuv_std;
    s_strip_out_rect(h, a_abs - c->y, b_abs - c->y,
                     &op.out.block_offset_x, &op.out.block_offset_y);

    op.rotation_angle = h->cur.t.rotation;
    op.scale_x        = h->cur.t.scale_x;
    op.scale_y        = h->cur.t.scale_y;
    op.mirror_x       = h->cur.t.mirror_x;
    op.mirror_y       = h->cur.t.mirror_y;
    op.rgb_swap       = h->cur.t.rgb_swap;
    op.byte_swap      = h->cur.t.byte_swap;
    op.mode           = PPA_TRANS_MODE_BLOCKING;

    return ppa_do_scale_rotate_mirror(h->ppa_client, &op);
}

static void s_worker_entry(void *arg)
{
    jpeg_ppa_pipeline_handle_t h = (jpeg_ppa_pipeline_handle_t)arg;
    while (true) {
        jpeg_enh_strip_event_t evt;
        if (xQueueReceive(h->worker_queue, &evt, portMAX_DELAY) != pdTRUE) continue;
        if (evt.strip_idx == WORKER_EXIT_MARKER) break;

        if (!h->abort_frame) {
            esp_err_t err = s_process_strip(h, &evt);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "ppa strip %lu err=%s", (unsigned long)evt.strip_idx, esp_err_to_name(err));
                if (h->frame_err == ESP_OK) h->frame_err = err;
            }
        }

        // Free this strip's ring slot for later strips of the same frame
        // (no-op for the trailing ring_count strips and on aborted frames).
        jpeg_enh_strip_decoder_release_strip(h->decoder, evt.strip_idx);

        if (__atomic_sub_fetch(&h->pending_strips, 1, __ATOMIC_ACQ_REL) == 0) {
            xSemaphoreGive(h->all_done);
        }
    }
    xSemaphoreGive(h->worker_joined);
    vTaskDelete(NULL);
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

esp_err_t jpeg_ppa_pipeline_new(const jpeg_ppa_pipeline_cfg_t *cfg,
                                jpeg_ppa_pipeline_handle_t *out_handle)
{
    if (!cfg || !out_handle) return ESP_ERR_INVALID_ARG;
    if (cfg->ring_count == 0) return ESP_ERR_INVALID_ARG;
    jpeg_dec_output_format_t jpeg_fmt = s_jpeg_out_for_strip_cm(cfg->strip_color_mode);
    if ((int)jpeg_fmt == -1) {
        ESP_LOGE(TAG, "unsupported strip color mode 0x%lx", (unsigned long)cfg->strip_color_mode);
        return ESP_ERR_INVALID_ARG;
    }

    jpeg_ppa_pipeline_handle_t h = heap_caps_calloc(1, sizeof(*h), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!h) return ESP_ERR_NO_MEM;
    h->cfg = *cfg;

    // Worst-case strips per frame: max_pic_h padded to MCU rows, divided by
    // the smallest possible strip height (one 8-row MCU).
    uint32_t max_strips = (cfg->max_pic_h + 15) / 16 * 16 / 8 + 1;
    h->worker_queue = xQueueCreate(max_strips, sizeof(jpeg_enh_strip_event_t));
    h->all_done = xSemaphoreCreateBinary();
    h->worker_joined = xSemaphoreCreateBinary();
    if (!h->worker_queue || !h->all_done || !h->worker_joined) {
        jpeg_ppa_pipeline_del(h);
        return ESP_ERR_NO_MEM;
    }

    // PPA SRM client: queue depth = ring_count keeps the strip decoder
    // backpressure simple.
    ppa_client_config_t pcfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = cfg->ring_count,
    };
    esp_err_t err = ppa_register_client(&pcfg, &h->ppa_client);
    if (err != ESP_OK) { jpeg_ppa_pipeline_del(h); return err; }

    jpeg_enh_strip_decoder_cfg_t dcfg = {
        .decode = {
            .output_format  = jpeg_fmt,
            .rgb_order      = cfg->rgb_order,
            .conv_std       = cfg->conv_std,
            .yuv_full_range = cfg->yuv_full_range,
        },
        .max_pic_w        = cfg->max_pic_w,
        .max_pic_h        = cfg->max_pic_h,
        .strip_h_hint     = cfg->strip_h_hint,
        .ring_count       = cfg->ring_count,
        .strip_alloc_caps = cfg->strip_alloc_caps,
        .on_frame_start   = s_on_frame_start,
        .on_strip_done    = s_on_strip_done_isr,
        .user_ctx         = h,
    };
    err = jpeg_enh_strip_decoder_new(&dcfg, &h->decoder);
    if (err != ESP_OK) { jpeg_ppa_pipeline_del(h); return err; }

    uint32_t stack = cfg->worker_stack_size ? cfg->worker_stack_size : DEFAULT_WORKER_STACK;
    uint32_t prio  = cfg->worker_priority ? cfg->worker_priority : DEFAULT_WORKER_PRIO;
    BaseType_t core = cfg->worker_core < 0 ? tskNO_AFFINITY : cfg->worker_core;
    if (xTaskCreatePinnedToCore(s_worker_entry, "jpeg_ppa_w", stack, h, prio,
                                &h->worker_task, core) != pdPASS) {
        jpeg_ppa_pipeline_del(h);
        return ESP_ERR_NO_MEM;
    }

    *out_handle = h;
    return ESP_OK;
}

esp_err_t jpeg_ppa_pipeline_del(jpeg_ppa_pipeline_handle_t h)
{
    if (!h) return ESP_OK;
    if (h->worker_task) {
        jpeg_enh_strip_event_t stop = { .strip_idx = WORKER_EXIT_MARKER };
        xQueueSend(h->worker_queue, &stop, portMAX_DELAY);
        xSemaphoreTake(h->worker_joined, portMAX_DELAY);
        h->worker_task = NULL;
    }
    if (h->decoder)      { jpeg_enh_strip_decoder_del(h->decoder); h->decoder = NULL; }
    if (h->ppa_client)   { ppa_unregister_client(h->ppa_client); h->ppa_client = NULL; }
    if (h->worker_queue) { vQueueDelete(h->worker_queue); h->worker_queue = NULL; }
    if (h->all_done)     { vSemaphoreDelete(h->all_done); h->all_done = NULL; }
    if (h->worker_joined){ vSemaphoreDelete(h->worker_joined); h->worker_joined = NULL; }
    heap_caps_free(h);
    return ESP_OK;
}

esp_err_t jpeg_ppa_pipeline_process(jpeg_ppa_pipeline_handle_t h,
                                    const void *jpeg_data, size_t jpeg_size,
                                    const jpeg_ppa_output_t *out,
                                    const jpeg_ppa_transform_t *transform,
                                    jpeg_enh_frame_info_t *info)
{
    if (!h || !h->decoder || !jpeg_data || !out || !out->buffer) return ESP_ERR_INVALID_ARG;
    if (out->pic_w == 0 || out->pic_h == 0) return ESP_ERR_INVALID_ARG;

    h->cur.valid = false;
    h->cur.out = *out;
    if (h->cur.out.buffer_size == 0) {
        h->cur.out.buffer_size = (size_t)out->pic_w * out->pic_h * s_cm_bits(out->color_mode) / 8;
    }
    h->cur.t = transform ? *transform : (jpeg_ppa_transform_t){ 0 };
    if (h->cur.t.scale_x == 0.0f) h->cur.t.scale_x = 1.0f;
    if (h->cur.t.scale_y == 0.0f) h->cur.t.scale_y = 1.0f;

    h->pending_strips = 0;
    h->frame_err = ESP_OK;
    h->abort_frame = false;
    // Drain a stale completion (defensive; see the abort path below)
    xSemaphoreTake(h->all_done, 0);

    esp_err_t err = jpeg_enh_strip_decoder_process(h->decoder, (const uint8_t *)jpeg_data,
                                                   (uint32_t)jpeg_size, info);
    if (err != ESP_OK) {
        // Decode failed. Strips that never made it out of the decoder still
        // count against pending_strips; settle the balance ourselves and wait
        // for the worker to finish whatever was already queued, so the next
        // process() starts from a clean slate.
        h->abort_frame = true;
        if (h->cur.valid) {
            uint32_t undelivered = h->cur.frame.strip_count
                                   - jpeg_enh_strip_decoder_strips_delivered(h->decoder);
            if (undelivered == 0 ||
                __atomic_sub_fetch(&h->pending_strips, undelivered, __ATOMIC_ACQ_REL) != 0) {
                if (xSemaphoreTake(h->all_done, ABORT_DRAIN_TIMEOUT) != pdTRUE) {
                    ESP_LOGE(TAG, "worker did not drain after decode error");
                }
            }
        }
        return err;
    }

    // Wait for the PPA worker to finish the trailing strips before letting
    // the caller touch the frame buffer.
    xSemaphoreTake(h->all_done, portMAX_DELAY);
    return h->frame_err;
}
