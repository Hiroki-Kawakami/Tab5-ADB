#include "agent_preview.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstring>

#include "agent_client.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char* TAG = "agent_preview";

namespace {
constexpr int align16(int v) { return (v + 15) & ~15; }
}  // namespace

std::shared_ptr<AgentPreview> AgentPreview::create(lv_obj_t* image, int box_w,
                                                   int box_h) {
    return std::shared_ptr<AgentPreview>(new AgentPreview(image, box_w, box_h));
}

AgentPreview::AgentPreview(lv_obj_t* image, int box_w, int box_h)
    : image_(image),
      box_w_(box_w),
      box_h_(box_h),
      pad_w_(align16(box_w)),
      pad_h_(align16(box_h)) {
    // The whole-frame decoder writes the MCU-padded raster (pic_w × pic_h) and
    // requires a cache-line (64 B) aligned output buffer — size for the padded
    // box, align like jpeg_alloc_decoder_mem would. pad_*'s 16-multiples keep
    // the byte size a 64-multiple, so the decoder's cache-aligned sync length
    // never exceeds the buffer.
    buf_size_ = (size_t)pad_w_ * pad_h_ * 2;  // RGB565
    for (auto& buf : img_buf_) {
        buf = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(64, buf_size_, MALLOC_CAP_SPIRAM));
        if (buf) std::memset(buf, 0, buf_size_);
    }
    if (!img_buf_[0] || !img_buf_[1]) ESP_LOGE(TAG, "PSRAM alloc failed (%zu B x2)", buf_size_);
    dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc_.data = img_buf_[0];

    free_q_ = xQueueCreate(kSlots, sizeof(int));
    ready_q_ = xQueueCreate(1, sizeof(int));
    for (int i = 0; i < kSlots; ++i) xQueueSend(static_cast<QueueHandle_t>(free_q_), &i, 0);
    decode_done_ = xSemaphoreCreateBinary();
}

AgentPreview::~AgentPreview() {
    stop();  // joins the decode task before the buffers it writes are freed
    if (decode_done_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(decode_done_));
    if (ready_q_) vQueueDelete(static_cast<QueueHandle_t>(ready_q_));
    if (free_q_) vQueueDelete(static_cast<QueueHandle_t>(free_q_));
    for (auto& s : slots_) {
        if (s.buf) heap_caps_free(s.buf);
    }
    heap_caps_free(img_buf_[0]);
    heap_caps_free(img_buf_[1]);
    if (jpeg_) jpeg_enh_strip_decoder_del(jpeg_);
}

void AgentPreview::start() {
    if (!img_buf_[0] || !img_buf_[1]) return;
    auto link = app::agent_client().link();
    if (!link) return;  // not Ready — the caller gates on ensure_connected
    stopped_.store(false);

    // A previous run may have left a published-but-undecoded frame behind.
    int stale;
    while (xQueueReceive(static_cast<QueueHandle_t>(ready_q_), &stale, 0) == pdTRUE)
        xQueueSend(static_cast<QueueHandle_t>(free_q_), &stale, 0);
    present_pending_.store(false);

    if (!decode_task_) {
        decode_stop_.store(false);
        TaskHandle_t t = nullptr;
        // Core 1, priority 3 (below the adb reader / LVGL), like the screencap
        // preview's decoder — this is a background glance, not the mirror.
        xTaskCreatePinnedToCore(&AgentPreview::decode_trampoline, "agprev_decode",
                                8192, this, 3, &t, 1);
        decode_task_ = t;
    }

    link->set_video_listener(weak_from_this());
    agent_link::MirrorConfig cfg;
    cfg.target_width = static_cast<uint16_t>(box_w_);
    cfg.target_height = static_cast<uint16_t>(box_h_);
    cfg.scale_mode = agent_link::kScaleAspect;  // agent sizes to the source aspect
    cfg.max_fps = 10;                           // keep the preview light on the link
    cfg.jpeg_quality = 60;                      // a glance, not the mirror (its 80)
    cfg.split_count = 1;                        // whole frame as one JPEG (§5.3)
    link->start_mirror(cfg);
}

void AgentPreview::stop() {
    if (stopped_.exchange(true)) return;
    // Stop the stream but KEEP the agent link (the AgentClient contract); clearing
    // the video listener stops any frame still in flight from reaching us.
    if (auto link = app::agent_client().link()) {
        link->stop_mirror();
        link->set_video_listener({});
    }
    if (decode_task_) {
        decode_stop_.store(true);
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(decode_done_), portMAX_DELAY);
        decode_task_ = nullptr;
    }
    // present() pointed image_'s source at our dsc_ / img_buf_. The image outlives
    // this preview (it belongs to the screen), so drop that reference before the
    // dtor frees the buffers — otherwise a redraw after we're gone reads freed
    // memory (the same "drop the src before freeing the pixels" rule as the media
    // bitmaps). The decode task is joined above, so no present() will re-set it.
    if (image_) lv_image_set_src(image_, nullptr);
}

// ---------------------------------------------------------------------------
// agent_link::VideoListener — adb reader thread (producer)
// ---------------------------------------------------------------------------

void AgentPreview::on_mirror_started(agent_link::Link*,
                                     const agent_link::MirrorInfo& info) {
    // The aspect-mode output size the agent chose (out_* == target for fit/fill;
    // 0 from a pre-out-dims agent falls back to the requested box).
    out_w_ = info.out_width > 0 ? info.out_width : box_w_;
    out_h_ = info.out_height > 0 ? info.out_height : box_h_;
}

void AgentPreview::on_video_strip(agent_link::Link*,
                                  const agent_link::VideoStrip& strip) {
    // Only copy + publish here (the mirror screen's producer): the decode runs on
    // its own task so this never stalls the reader / the stream flow control.
    // split_count=1, so a frame is exactly one whole-frame strip; anything else
    // (a stale multi-strip config) is dropped per frame, not per stream.
    if (!strip.frame_start || !strip.frame_end || strip.x != 0 || strip.y != 0 ||
        strip.w == 0 || strip.h == 0 || strip.jpeg_len == 0 ||
        strip.w != out_w_ || strip.h != out_h_ ||
        align16(strip.w) > pad_w_ || align16(strip.h) > pad_h_) {
        return;
    }

    int slot;
    if (xQueueReceive(static_cast<QueueHandle_t>(free_q_), &slot, 0) != pdTRUE)
        return;  // no free slot: drop the frame (latest wins)

    FrameSlot& f = slots_[slot];
    if (strip.jpeg_len > f.cap) {
        uint32_t cap = (static_cast<uint32_t>(strip.jpeg_len) + 0x7FFFu) & ~0x7FFFu;
        auto* nb = static_cast<uint8_t*>(heap_caps_realloc(f.buf, cap, MALLOC_CAP_SPIRAM));
        if (!nb) {
            xQueueSend(static_cast<QueueHandle_t>(free_q_), &slot, 0);
            return;
        }
        f.buf = nb;
        f.cap = cap;
    }
    std::memcpy(f.buf, strip.jpeg, strip.jpeg_len);
    f.len = strip.jpeg_len;
    f.fw = strip.w;
    f.fh = strip.h;

    // Publish (latest-frame-wins): reclaim a still-queued stale frame's slot.
    int stale;
    if (xQueueReceive(static_cast<QueueHandle_t>(ready_q_), &stale, 0) == pdTRUE)
        xQueueSend(static_cast<QueueHandle_t>(free_q_), &stale, 0);
    xQueueSend(static_cast<QueueHandle_t>(ready_q_), &slot, 0);
}

// ---------------------------------------------------------------------------
// decode task (consumer)
// ---------------------------------------------------------------------------

void AgentPreview::decode_trampoline(void* arg) {
    static_cast<AgentPreview*>(arg)->decode_loop();
}

void AgentPreview::decode_loop() {
    while (!decode_stop_.load()) {
        int slot;
        if (xQueueReceive(static_cast<QueueHandle_t>(ready_q_), &slot,
                          pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        // The other buffer's flip may still be queued on the LVGL thread; don't
        // overwrite the one buffer LVGL might still be reading from.
        while (present_pending_.load() && !decode_stop_.load())
            vTaskDelay(pdMS_TO_TICKS(5));
        if (decode_stop_.load()) {
            xQueueSend(static_cast<QueueHandle_t>(free_q_), &slot, 0);
            break;
        }

        FrameSlot& f = slots_[slot];
        if (!jpeg_) {
            jpeg_enh_strip_decoder_cfg_t cfg = {};
            // RGB565 with the BGR scramble = LVGL's R-in-high-bits packing (the
            // same choice as the mirror / screencap decoders); full-range JFIF.
            cfg.decode.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
            cfg.decode.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
            cfg.decode.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
            cfg.decode.yuv_full_range = true;
            cfg.max_pic_w = static_cast<uint32_t>(pad_w_);
            cfg.max_pic_h = static_cast<uint32_t>(pad_h_);
            cfg.timeout_ms = 1000;
            if (jpeg_enh_strip_decoder_new(&cfg, &jpeg_) != ESP_OK) {
                jpeg_ = nullptr;
                xQueueSend(static_cast<QueueHandle_t>(free_q_), &slot, 0);
                continue;
            }
        }

        // Whole-frame decode into the back buffer. The decoder reports the
        // MCU-padded raster width (info.pic_w) — that is the row stride the
        // lv_image must use to show the real fw×fh frame.
        jpeg_enh_frame_info_t info = {};
        bool ok = jpeg_enh_decoder_process(jpeg_, f.buf, f.len, img_buf_[back_],
                                           buf_size_, &info) == ESP_OK &&
                  info.origin_w == f.fw && info.origin_h == f.fh;
        if (ok) {
            frame_w_[back_] = f.fw;
            frame_h_[back_] = f.fh;
            stride_[back_] = static_cast<int>(info.pic_w) * 2;
            const int idx = back_;
            back_ ^= 1;
            present_pending_.store(true);
            lv_async_call([weak = weak_from_this(), idx]() {
                if (auto self = weak.lock()) self->present(idx);
            });
        }
        xQueueSend(static_cast<QueueHandle_t>(free_q_), &slot, 0);
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(decode_done_));
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// presentation — LVGL thread
// ---------------------------------------------------------------------------

void AgentPreview::present(int idx) {
    present_pending_.store(false);  // the decode task may reuse the other buffer
    if (stopped_.load()) return;
    const int fw = frame_w_[idx], fh = frame_h_[idx];
    dsc_.header.w = fw;
    dsc_.header.h = fh;
    dsc_.header.stride = stride_[idx];
    dsc_.data = img_buf_[idx];
    dsc_.data_size = (size_t)stride_[idx] * fh;
    if (lv_obj_get_width(image_) != fw || lv_obj_get_height(image_) != fh)
        lv_obj_set_size(image_, fw, fh);
    lv_image_set_src(image_, &dsc_);
    lv_obj_invalidate(image_);
}
