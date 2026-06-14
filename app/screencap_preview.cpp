#include "screencap_preview.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "adb_app.hpp"
#include "adb_client.hpp"
#include "driver/jpeg_decode.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_ppa_pipeline.h"

static const char* TAG = "screencap_preview";

std::shared_ptr<ScreencapPreview> ScreencapPreview::create(lv_obj_t* image, int max_w, int max_h) {
    return std::shared_ptr<ScreencapPreview>(new ScreencapPreview(image, max_w, max_h));
}

ScreencapPreview::ScreencapPreview(lv_obj_t* image, int max_w, int max_h)
    : image_(image), max_w_(max_w), max_h_(max_h) {
    buf_size_ = (size_t)max_w_ * max_h_ * 2;  // RGB565, sized for the bounding box
    img_buf_[0] = static_cast<uint8_t*>(heap_caps_calloc(buf_size_, 1, MALLOC_CAP_SPIRAM));
    img_buf_[1] = static_cast<uint8_t*>(heap_caps_calloc(buf_size_, 1, MALLOC_CAP_SPIRAM));
    if (!img_buf_[0] || !img_buf_[1]) ESP_LOGE(TAG, "PSRAM alloc failed (%zu B x2)", buf_size_);
    dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    // w/h/stride/data are per-frame: present() points them at the freshly-decoded
    // aspect-fitted frame before the dsc_ is ever shown.
    dsc_.data = img_buf_[0];  // front; write_idx_ starts at 1 (the back)
}

ScreencapPreview::~ScreencapPreview() {
    stop();  // joins the decode task before the buffers it writes are freed
    if (work_sem_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(work_sem_));
    if (decode_done_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(decode_done_));
    heap_caps_free(img_buf_[0]);
    heap_caps_free(img_buf_[1]);
}

void ScreencapPreview::start() {
    stopped_.store(false);
    decode_stop_.store(false);
    if (!decode_task_) {
        work_sem_ = xSemaphoreCreateBinary();
        decode_done_ = xSemaphoreCreateBinary();
        TaskHandle_t t = nullptr;
        // Core 1, priority 3 (below the prio-5 adb reader / LVGL) so the heavy PNG
        // inflate+downscale never preempts them — this is a rough, low-rate preview.
        xTaskCreatePinnedToCore(&ScreencapPreview::decode_trampoline, "scap_decode",
                                8192, this, 3, &t, 1);
        decode_task_ = t;
    }
    capture_once();
}

void ScreencapPreview::stop() {
    stopped_.store(true);
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (stream_) {
        stream_->close();
        stream_.reset();
    }
    if (decode_task_) {
        decode_stop_.store(true);
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(work_sem_));        // wake to exit
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(decode_done_), portMAX_DELAY);
        decode_task_ = nullptr;  // sems freed in the dtor (a late on_stream_close may
                                 // still give work_sem_ while the weak listener is locked)
    }
    // The decode task (sole owner of the pipeline) is now joined, so it's safe to
    // release the JPEG HW engine / PPA client / ring buffers here.
    if (jpeg_pipe_) {
        jpeg_ppa_pipeline_del(static_cast<jpeg_ppa_pipeline_handle_t>(jpeg_pipe_));
        jpeg_pipe_ = nullptr;
        jpeg_pipe_max_ = 0;
    }
}

void ScreencapPreview::capture_once() {
    if (stopped_.load() || !img_buf_[0] || !img_buf_[1]) return;
    capturing_ = true;
    auto* client = app::adb_client();
    if (!client || client->state() != adb::ConnectionState::Online) {
        capturing_ = false;
        schedule_next();  // retry after the interval
        return;
    }
    png_.clear();
    t_capture_us_ = esp_timer_get_time();
    // exec: (not shell:) — binary-safe, no PTY CR/LF translation of the image bytes.
    // -j (JPEG, much smaller) on capable devices; -p (PNG) otherwise.
    const char* service = use_jpeg_.load() ? "exec:screencap -j" : "exec:screencap -p";
    stream_ = client->open_stream(
        service, std::weak_ptr<adb::StreamListener>(shared_from_this()));
    if (!stream_) {
        capturing_ = false;
        schedule_next();
    }
}

void ScreencapPreview::schedule_next() {
    if (stopped_.load() || timer_) return;
    timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<ScreencapPreview*>(lv_timer_get_user_data(t));
            lv_timer_delete(t);
            self->timer_ = nullptr;
            self->capture_once();
        },
        interval_ms_, this);
}

void ScreencapPreview::present(int idx, bool ok) {
    if (stopped_.load() || !ok) return;  // failed decode: keep showing the last frame
    // Flip dsc_ to the buffer the decode task just filled (no copy); the next decode
    // then targets the other buffer (the one LVGL is done showing). The frame is a
    // tightly-packed aspect-fitted frame_w_*frame_h_ image, and the lv_image is
    // resized to hug it (the box stays letterbox-free across devices / rotation).
    const int fw = frame_w_[idx], fh = frame_h_[idx];
    dsc_.header.w = fw;
    dsc_.header.h = fh;
    dsc_.header.stride = fw * 2;
    dsc_.data = img_buf_[idx];
    dsc_.data_size = (size_t)fw * fh * 2;
    write_idx_.store(idx ^ 1);
    if (fw != frame_w_[idx ^ 1] || fh != frame_h_[idx ^ 1])  // vs the frame shown so far
        lv_obj_set_size(image_, fw, fh);
    lv_image_set_src(image_, &dsc_);
    lv_obj_invalidate(image_);

    // Per-stage timing (µs->ms) for tuning. xfer = phone screencap + USB transfer;
    // wait = decode task scheduling latency (low prio, Core 1); decode = inflate +
    // downscale; total = capture open -> shown.
    int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG, "preview %s %dx%d->%dx%d %ukB | xfer %lldms wait %lldms decode %lldms total %lldms",
             last_fmt_, src_w_, src_h_, fw, fh, (unsigned)(png_bytes_ / 1024),
             (long long)((t_recv_us_ - t_capture_us_) / 1000),
             (long long)((t_dec_start_us_ - t_recv_us_) / 1000),
             (long long)((t_dec_end_us_ - t_dec_start_us_) / 1000),
             (long long)((now - t_capture_us_) / 1000));
}

void ScreencapPreview::on_stream_data(adb::Stream*, const uint8_t* data, size_t len) {
    png_.insert(png_.end(), data, data + len);
}

void ScreencapPreview::on_stream_close(adb::Stream*, adb::Error) {
    // Reader thread (high priority): just hand the captured PNG to the decode task
    // and return — decode/downscale must not run here or it blocks the reader (and
    // thus LVGL / other adb streams). The pipeline is serial (next capture only
    // arms 2 s after present), so png_ stays stable while the task reads it.
    t_recv_us_ = esp_timer_get_time();
    png_bytes_ = png_.size();
    if (work_sem_) xSemaphoreGive(static_cast<SemaphoreHandle_t>(work_sem_));
}

bool ScreencapPreview::decode_jpeg(const uint8_t* data, size_t len, int idx) {
    jpeg_decode_picture_info_t pi;
    if (jpeg_decoder_get_info(data, len, &pi) != ESP_OK) return false;

    // Lazy pipeline, sized to cover both orientations of this device (max(W,H) on
    // both axes, MCU-aligned) so a portrait<->landscape rotation needs no rebuild.
    uint32_t maxdim = (std::max(pi.width, pi.height) + 15u) & ~15u;
    if (!jpeg_pipe_ || jpeg_pipe_max_ < maxdim) {
        if (jpeg_pipe_) jpeg_ppa_pipeline_del(static_cast<jpeg_ppa_pipeline_handle_t>(jpeg_pipe_));
        jpeg_pipe_ = nullptr;
        jpeg_ppa_pipeline_cfg_t cfg = {};
        cfg.max_pic_w = maxdim;
        cfg.max_pic_h = maxdim;
        cfg.strip_h_hint = 16;
        cfg.ring_count = 4;
        cfg.strip_color_mode = PPA_SRM_COLOR_MODE_RGB565;
        cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;  // R-high RGB565 for LVGL
        cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
        cfg.yuv_full_range = true;                       // JFIF/UltraHDR base is full-range
        cfg.strip_alloc_caps = MALLOC_CAP_SPIRAM;        // PSRAM ring; rate isn't critical
        cfg.worker_core = 1;
        cfg.worker_priority = 3;                         // low, like our decode task
        jpeg_ppa_pipeline_handle_t p = nullptr;
        if (jpeg_ppa_pipeline_new(&cfg, &p) != ESP_OK) {
            ESP_LOGW(TAG, "jpeg pipeline new failed (%ux%u)", (unsigned)pi.width,
                     (unsigned)pi.height);
            return false;
        }
        jpeg_pipe_ = p;
        jpeg_pipe_max_ = maxdim;
    }

    // Aspect-fit into the bounding box, quantizing the scale down to PPA's 1/16
    // step; the quantized extent *is* the output frame (no letterbox — the
    // lv_image is resized to the frame in present()).
    float s = std::min((float)max_w_ / pi.width, (float)max_h_ / pi.height);
    float q = std::floor(s * 16.0f) / 16.0f;
    if (q < 1.0f / 16) q = 1.0f / 16;
    uint32_t ew = (uint32_t)(pi.width * q), eh = (uint32_t)(pi.height * q);
    if (ew < 1) ew = 1;
    if (eh < 1) eh = 1;

    jpeg_ppa_transform_t t = {};
    t.rotation = PPA_SRM_ROTATION_ANGLE_0;
    t.scale_x = t.scale_y = q;

    jpeg_ppa_output_t out = {};
    out.buffer = img_buf_[idx];
    out.buffer_size = buf_size_;
    out.pic_w = ew;
    out.pic_h = eh;
    out.color_mode = PPA_SRM_COLOR_MODE_RGB565;

    esp_err_t e = jpeg_ppa_pipeline_process(
        static_cast<jpeg_ppa_pipeline_handle_t>(jpeg_pipe_), data, len, &out, &t, nullptr);
    if (e != ESP_OK) { ESP_LOGW(TAG, "jpeg pipeline process: %d", (int)e); return false; }
    frame_w_[idx] = (int)ew;
    frame_h_[idx] = (int)eh;
    src_w_ = pi.width;
    src_h_ = pi.height;
    return true;
}

void ScreencapPreview::decode_trampoline(void* arg) {
    static_cast<ScreencapPreview*>(arg)->decode_loop();
}

void ScreencapPreview::decode_loop() {
    for (;;) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(work_sem_), portMAX_DELAY);
        if (decode_stop_.load()) break;

        t_dec_start_us_ = esp_timer_get_time();
        int idx = write_idx_.load();  // the back buffer (front = idx ^ 1, shown by LVGL)
        const uint8_t* d = png_.data();
        size_t n = png_.size();
        bool is_jpeg = n >= 2 && d[0] == 0xFF && d[1] == 0xD8;
        bool is_png = n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
        bool ok;
        if (is_jpeg) {
            last_fmt_ = "jpeg";
            ok = decode_jpeg(d, n, idx);
        } else {
            last_fmt_ = "png";
            ok = is_png && app::decode_png_downscale_rgb565(
                              d, n, reinterpret_cast<uint16_t*>(img_buf_[idx]),
                              max_w_, max_h_, &frame_w_[idx], &frame_h_[idx],
                              &src_w_, &src_h_);
        }
        // We asked for `-j` but got something else (or nothing) → the device lacks
        // it; drop to PNG for the rest of the session.
        if (use_jpeg_.load() && !is_jpeg) {
            use_jpeg_.store(false);
            ESP_LOGW(TAG, "screencap -j unsupported here; falling back to PNG");
        }
        t_dec_end_us_ = esp_timer_get_time();
        png_.clear();
        if (!ok) ESP_LOGW(TAG, "decode failed");

        // weak, not shared_from_this(): an adb disconnect can hand us work while
        // the last owner is already in ~ScreencapPreview (stop() joins this task
        // from the dtor), where shared_from_this() throws bad_weak_ptr. The lock
        // runs on the LVGL thread, serialized with the dtor, so an expired weak
        // just skips the dead object's presentation.
        lv_async_call([weak = weak_from_this(), idx, ok]() {
            auto self = weak.lock();
            if (!self) return;
            self->capturing_ = false;
            self->stream_.reset();
            self->present(idx, ok);
            self->schedule_next();
        });
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(decode_done_));
    vTaskDelete(nullptr);
}
