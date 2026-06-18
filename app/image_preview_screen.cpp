#include "image_preview_screen.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "adb_app.hpp"
#include "adb_file_browser_screen.hpp"
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_decode_enhanced.h"
#include "modal.hpp"
#include "resources/resources.h"
#include "screen_manager.hpp"
#include "sd_file_browser_screen.hpp"

namespace {
const char* TAG = "image_preview";
constexpr size_t kReadChunk = 16 * 1024;  // SD fast-path chunk (cache-aligned buffer)

constexpr uint32_t align16(uint32_t v) { return (v + 15u) & ~15u; }

bool adb_online() {
    adb::Client* c = app::adb_client();
    return c && c->state() == adb::ConnectionState::Online;
}
}  // namespace

ImagePreviewScreen::~ImagePreviewScreen() {
    stop_engine();  // joins the decode task before its buffers are freed
    if (work_sem_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(work_sem_));
    if (decode_done_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(decode_done_));
    if (img_buf_) heap_caps_free(img_buf_);
}

void ImagePreviewScreen::onExit() {
    stop_engine();
    if (job_) job_->abort();  // the copy progress modal dies with this screen
}

void ImagePreviewScreen::stop_engine() {
    if (sync_) {
        sync_->close();  // joins the Sync worker — no more RECV sink after this
        sync_.reset();
    }
    if (decode_task_) {
        decode_stop_.store(true);
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(work_sem_));  // wake to exit
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(decode_done_), portMAX_DELAY);
        decode_task_ = nullptr;  // sems freed in the dtor (a late on_stream_close
                                 // may still give work_sem_ while the screen is alive)
    }
    // The decode task (sole owner of the JPEG decoder) is joined — release here.
    if (jpeg_dec_) {
        jpeg_enh_strip_decoder_del(static_cast<jpeg_enh_strip_decoder_handle_t>(jpeg_dec_));
        jpeg_dec_ = nullptr;
        jpeg_dec_max_ = 0;
    }
    if (decode_buf_) {
        heap_caps_free(decode_buf_);
        decode_buf_ = nullptr;
        decode_buf_size_ = 0;
    }
}

void ImagePreviewScreen::build() {
    lv_obj_t* content = nullptr;
    app::preview_chrome(this, "Image", &content);

    // The SD listing carries no metadata — stat the local file. The Android side
    // passes the DirEntry's size/mtime in.
    if (ref_.where == app::FileRef::Where::SD) {
        struct stat st = {};
        if (stat(ref_.path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            ref_.size = (uint32_t)st.st_size;
            ref_.mtime = (uint32_t)st.st_mtime;
        }
    }

    app::preview_header(content, LUCIDE_IMAGE, ref_.name());

    // The image stage: fills the column, centers the spinner / image / error.
    stage_ = lv_obj_create(content);
    lv_obj_remove_style_all(stage_);
    lv_obj_set_width(stage_, LV_PCT(100));
    lv_obj_set_height(stage_, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(stage_, 360, 0);
    lv_obj_remove_flag(stage_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(stage_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(stage_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(stage_, 12, 0);

    auto actions = lv_obj_create(content);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(actions, 16, 0);
    rebuild_actions(actions);

    buf_size_ = (size_t)kMaxW * kMaxH * 2;  // RGB565, sized for the bounding box
    img_buf_ = static_cast<uint8_t*>(heap_caps_calloc(buf_size_, 1, MALLOC_CAP_SPIRAM));
    if (!img_buf_) ESP_LOGE(TAG, "PSRAM alloc failed (%zu B)", buf_size_);

    start_load();
}

void ImagePreviewScreen::rebuild_actions(lv_obj_t* box) {
    if (ref_.where == app::FileRef::Where::Android) {
        app::preview_action(box, LUCIDE_HARD_DRIVE_DOWNLOAD, "Copy to SD Card", true,
                            [this](lv_event_t*) { copy_to_sd(); });
    } else {
        bool online = adb_online();
        app::preview_action(box, LUCIDE_SMARTPHONE,
                            online ? "Copy to Android" : "Copy to Android (not connected)",
                            online, [this](lv_event_t*) { copy_to_android(); });
    }
}

void ImagePreviewScreen::copy_to_sd() {
    auto weak = std::weak_ptr<ImagePreviewScreen>(
        std::static_pointer_cast<ImagePreviewScreen>(shared_from_this()));
    screen_manager.push(std::make_shared<SDFileBrowserScreen>(SDFileBrowserScreen::PickDir{
        "Copy Here", [weak](const std::string& dir) {
            if (auto self = weak.lock(); self && !self->exited())
                self->job_ = app::pull_to_sd(self->root_, self->ref_.path, self->ref_.size,
                                             dir, {});
        }}));
}

void ImagePreviewScreen::copy_to_android() {
    auto weak = std::weak_ptr<ImagePreviewScreen>(
        std::static_pointer_cast<ImagePreviewScreen>(shared_from_this()));
    screen_manager.push(std::make_shared<ADBFileBrowserScreen>(
        "/sdcard", ADBFileBrowserScreen::PickDir{
                       "Copy Here", [weak](const std::string& dir) {
                           if (auto self = weak.lock(); self && !self->exited())
                               self->job_ = app::push_to_android(self->root_, self->ref_.path,
                                                                 dir, {});
                       }}));
}

void ImagePreviewScreen::start_load() {
    // Spinner in the stage while the picture loads + decodes.
    oversize_ = false;
    image_ = nullptr;
    lv_obj_clean(stage_);
    auto spinner = lv_spinner_create(stage_);
    lv_obj_set_size(spinner, 80, 80);
    auto label = lv_label_create(stage_);
    lv_label_set_text(label, "Loading...");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x888888), 0);

    if (!img_buf_) {
        present(false);
        return;
    }

    if (!decode_task_) {
        work_sem_ = xSemaphoreCreateBinary();
        decode_done_ = xSemaphoreCreateBinary();
        TaskHandle_t t = nullptr;
        // Core 1, priority 3 (below the prio-5 adb reader / LVGL) so the heavy
        // inflate / JPEG decode never preempts them.
        xTaskCreatePinnedToCore(&ImagePreviewScreen::decode_trampoline, "img_decode",
                                8192, this, 3, &t, 1);
        decode_task_ = t;
    }

    // Pre-flight: the whole compressed file is held in one contiguous PSRAM block
    // (both decoders need the full buffer). The PsramAllocator vector aborts on
    // OOM (-fno-exceptions can't throw), so refuse oversize files here instead of
    // crashing. Check the largest free block, not total free — fragmentation at
    // this nav depth is what bounds us. Headroom covers the decoders' own scratch.
    constexpr size_t kHeadroom = 1024 * 1024;  // 1 MB
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (ref_.size && (size_t)ref_.size + kHeadroom > largest) {
        ESP_LOGW(TAG, "image too large: %u B, largest free PSRAM block %zu B",
                 (unsigned)ref_.size, largest);
        oversize_ = true;
        present(false);
        return;
    }

    if (ref_.where == app::FileRef::Where::SD) {
        // The file read happens on the decode task (local FS, fast).
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(work_sem_));
        return;
    }

    // Android: pull the file over the sync: RECV service — the proven transfer
    // path (file_transfer / the agent-jar push use the same one on the real Tab5).
    if (!adb_online()) {
        present(false);
        return;
    }
    data_.clear();
    // Reserve the exact file size up front so the sink's appends never trigger a
    // geometric-growth realloc (which would briefly need ~2x the file in one
    // contiguous PSRAM block — fatal for a multi-MB image on a fragmented heap).
    if (ref_.size) data_.reserve(ref_.size);
    auto self = std::static_pointer_cast<ImagePreviewScreen>(shared_from_this());
    sync_ = app::adb_client()->open_sync(std::weak_ptr<adb::SyncListener>(self));
    if (!sync_) {
        present(false);
        return;
    }
    auto weak = std::weak_ptr<ImagePreviewScreen>(self);
    sync_->pull(
        ref_.path,
        [weak](const uint8_t* d, size_t n) -> bool {  // Sync worker thread
            auto s = weak.lock();
            if (!s) return false;  // screen gone -> abort the RECV
            s->data_.insert(s->data_.end(), d, d + n);
            return true;
        },
        [weak](adb::Error err) {  // Sync worker thread
            auto s = weak.lock();
            if (!s) return;
            if (err == adb::Error::Ok) {
                xSemaphoreGive(static_cast<SemaphoreHandle_t>(s->work_sem_));  // -> decode
                return;
            }
            ESP_LOGW(TAG, "pull %s failed: %s", s->ref_.path.c_str(), adb::to_string(err));
            lv_async_call([weak]() {
                auto s2 = weak.lock();
                if (!s2 || s2->exited()) return;
                s2->present(false);
            });
        });
}

void ImagePreviewScreen::present(bool ok) {
    if (exited()) return;
    lv_obj_clean(stage_);
    image_ = nullptr;

    if (ok) {
        image_ = lv_image_create(stage_);
        dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
        dsc_.header.w = frame_w_;
        dsc_.header.h = frame_h_;
        dsc_.header.stride = frame_w_ * 2;
        dsc_.data = img_buf_;
        dsc_.data_size = (size_t)frame_w_ * frame_h_ * 2;
        lv_image_set_src(image_, &dsc_);
        lv_obj_set_size(image_, frame_w_, frame_h_);

        auto caption = lv_label_create(stage_);
        char buf[64];
        snprintf(buf, sizeof(buf), "%d x %d  -  %s", src_w_, src_h_,
                 app::format_size(ref_.size).c_str());
        lv_label_set_text(caption, buf);
        lv_obj_set_style_text_font(caption, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(caption, lv_color_hex(0x888888), 0);
    } else {
        auto label = lv_label_create(stage_);
        const char* msg = "Cannot display this image.";
        if (ref_.where == app::FileRef::Where::Android && !adb_online())
            msg = "Not connected.";
        else if (oversize_)
            msg = "Image too large to display.";
        lv_label_set_text(label, msg);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x888888), 0);
    }
}

void ImagePreviewScreen::decode_trampoline(void* arg) {
    static_cast<ImagePreviewScreen*>(arg)->decode_loop();
}

bool ImagePreviewScreen::load_sd_file() {
    int fd = ::open(ref_.path.c_str(), O_RDONLY);
    if (fd < 0) {
        ESP_LOGW(TAG, "open %s failed: %s", ref_.path.c_str(), strerror(errno));
        return false;
    }
    // Prefer the internal DMA buffer (the fast, proven SD read path); fall back to
    // PSRAM when internal DMA RAM is exhausted (esp-hosted/WiFi hold most of it at
    // this nav depth) — FATFS bounces the PSRAM destination through its own window.
    uint8_t* buf = (uint8_t*)heap_caps_malloc(kReadChunk, MALLOC_CAP_CACHE_ALIGNED);
    if (!buf)
        buf = (uint8_t*)heap_caps_malloc(kReadChunk,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!buf) {
        ESP_LOGW(TAG, "read buffer alloc failed (%zu B)", kReadChunk);
        ::close(fd);
        return false;
    }
    data_.clear();
    if (ref_.size) data_.reserve(ref_.size);  // exact size -> no doubling realloc
    bool ok = true;
    for (;;) {
        ssize_t r = ::read(fd, buf, kReadChunk);
        if (r < 0) {
            ESP_LOGW(TAG, "read %s failed: %s", ref_.path.c_str(), strerror(errno));
            ok = false;
            break;
        }
        if (r == 0) break;
        data_.insert(data_.end(), buf, buf + r);
    }
    heap_caps_free(buf);
    ::close(fd);
    if (ok && data_.empty()) ESP_LOGW(TAG, "%s is empty", ref_.path.c_str());
    return ok && !data_.empty();
}

void ImagePreviewScreen::decode_loop() {
    for (;;) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(work_sem_), portMAX_DELAY);
        if (decode_stop_.load()) break;

        bool ok = ref_.where == app::FileRef::Where::SD ? load_sd_file() : !data_.empty();
        if (ok) {
            const uint8_t* d = data_.data();
            size_t n = data_.size();
            bool is_png = n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
            bool is_jpeg = n >= 2 && d[0] == 0xFF && d[1] == 0xD8;
            if (is_jpeg)
                ok = decode_jpeg(d, n);
            else if (is_png)
                ok = decode_png(d, n);
            else
                ok = false;
        }
        if (ok)
            ESP_LOGI(TAG, "image %dx%d -> %dx%d %zukB", src_w_, src_h_, frame_w_, frame_h_,
                     data_.size() / 1024);
        else
            ESP_LOGW(TAG, "image decode failed (%zu B)", data_.size());
        // Free the compressed bytes — they're decoded into img_buf_ now.
        data_.clear();
        data_.shrink_to_fit();

        // weak, not shared_from_this(): an adb disconnect / teardown can give us
        // work while the screen is in its dtor (stop_engine joins this task there),
        // where shared_from_this() throws. The lock runs on the LVGL thread,
        // serialized with the dtor, so an expired weak just skips a dead screen.
        lv_async_call([weak = weak_from_this(), ok]() {
            auto base = weak.lock();
            if (!base) return;
            auto self = std::static_pointer_cast<ImagePreviewScreen>(base);
            if (self->exited()) return;
            self->present(ok);
        });
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(decode_done_));
    vTaskDelete(nullptr);
}

bool ImagePreviewScreen::decode_png(const uint8_t* data, size_t len) {
    return app::decode_png_downscale_rgb565(data, len, reinterpret_cast<uint16_t*>(img_buf_),
                                            kMaxW, kMaxH, &frame_w_, &frame_h_, &src_w_,
                                            &src_h_);
}

bool ImagePreviewScreen::decode_jpeg(const uint8_t* data, size_t len) {
    jpeg_decode_picture_info_t pi;
    if (jpeg_decoder_get_info(data, len, &pi) != ESP_OK) {
        ESP_LOGW(TAG, "jpeg header parse failed");
        return false;
    }

    // Whole-frame jpeg_decode_enhanced decoder (Layer 1, ring_count=0): the JPEG
    // decoder WITHOUT the PPA pipeline. The PPA pipeline registers its PPA client
    // (internal DMA) before creating the engine, which fails for lack of internal
    // RAM here; the bare decoder matches the mirror's footprint, which works.
    uint32_t pw = align16(pi.width), ph = align16(pi.height);
    uint32_t maxdim = std::max(pw, ph);
    if (!jpeg_dec_ || jpeg_dec_max_ < maxdim) {
        if (jpeg_dec_)
            jpeg_enh_strip_decoder_del(static_cast<jpeg_enh_strip_decoder_handle_t>(jpeg_dec_));
        jpeg_dec_ = nullptr;
        jpeg_enh_strip_decoder_cfg_t cfg = {};
        cfg.decode.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
        cfg.decode.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;  // R-high RGB565 for LVGL
        cfg.decode.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
        cfg.decode.yuv_full_range = true;  // JFIF base is full-range
        cfg.max_pic_w = pw;
        cfg.max_pic_h = ph;
        cfg.ring_count = 0;  // whole-frame mode (no PPA, no strip ring)
        cfg.timeout_ms = 2000;
        jpeg_enh_strip_decoder_handle_t d = nullptr;
        if (jpeg_enh_strip_decoder_new(&cfg, &d) != ESP_OK) {
            ESP_LOGW(TAG, "jpeg decoder new failed (%ux%u)", (unsigned)pi.width,
                     (unsigned)pi.height);
            return false;
        }
        jpeg_dec_ = d;
        jpeg_dec_max_ = maxdim;
    }

    // Full-res RGB565 decode target (MCU-padded, 64-byte aligned for the HW path).
    size_t need = (size_t)pw * ph * 2;
    if (decode_buf_size_ < need) {
        if (decode_buf_) heap_caps_free(decode_buf_);
        decode_buf_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(64, need, MALLOC_CAP_SPIRAM));
        decode_buf_size_ = decode_buf_ ? need : 0;
        if (!decode_buf_) {
            ESP_LOGW(TAG, "decode buffer alloc failed (%zu B)", need);
            return false;
        }
    }

    jpeg_enh_frame_info_t info = {};
    if (jpeg_enh_decoder_process(static_cast<jpeg_enh_strip_decoder_handle_t>(jpeg_dec_), data,
                                 len, decode_buf_, decode_buf_size_, &info) != ESP_OK) {
        ESP_LOGW(TAG, "jpeg decode process failed");
        return false;
    }

    // CPU nearest-neighbour downscale: origin_w×origin_h valid image (row stride =
    // the MCU-padded info.pic_w) -> the aspect-fitted img_buf_ frame.
    src_w_ = (int)info.origin_w;
    src_h_ = (int)info.origin_h;
    app::aspect_fit(src_w_, src_h_, kMaxW, kMaxH, &frame_w_, &frame_h_);
    const uint16_t* src = reinterpret_cast<const uint16_t*>(decode_buf_);
    const int stride_px = (int)info.pic_w;
    uint16_t* dst = reinterpret_cast<uint16_t*>(img_buf_);
    for (int y = 0; y < frame_h_; y++) {
        int sy = (int)(((2 * y + 1) * (long long)src_h_) / (2 * frame_h_));
        if (sy >= src_h_) sy = src_h_ - 1;
        const uint16_t* srow = src + (size_t)sy * stride_px;
        uint16_t* drow = dst + (size_t)y * frame_w_;
        for (int x = 0; x < frame_w_; x++) {
            int sx = (int)(((2 * x + 1) * (long long)src_w_) / (2 * frame_w_));
            if (sx >= src_w_) sx = src_w_ - 1;
            drow[x] = srow[sx];
        }
    }
    return true;
}
