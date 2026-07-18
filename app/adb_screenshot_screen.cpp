#include "adb_screenshot_screen.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "adb_app.hpp"
#include "bsp.h"
#include "file_preview.hpp"  // preview_chrome
#include "lvgl.hpp"
#include "modal.hpp"
#include "sysclock.hpp"
#include "resources/resources.h"

namespace {
const char* TAG = "screenshot";
constexpr const char* kMountPoint = "/sd";
constexpr size_t kWriteChunk = 16 * 1024;

// One SD save in flight. The writer task's closure holds the shared_ptr, so the
// task outliving the screen is safe; the dtor frees the snapshot buffer whenever
// the last ref drops.
struct ShotSaveJob {
    ~ShotSaveJob() {
        if (buf) heap_caps_free(buf);
    }
    uint8_t* buf = nullptr;
    size_t len = 0;
    std::string path;
};
}  // namespace

ADBScreenshotScreen::ADBScreenshotScreen() {
    buf_size_ = (size_t)kMaxW * kMaxH * 2;  // RGB565, sized for the bounding box
    img_buf_ = static_cast<uint8_t*>(heap_caps_calloc(buf_size_, 1, MALLOC_CAP_SPIRAM));
    if (!img_buf_) ESP_LOGE(TAG, "PSRAM alloc failed (%zu B)", buf_size_);
}

ADBScreenshotScreen::~ADBScreenshotScreen() {
    stop_engine();  // joins the decode task before its buffers are freed
    if (work_sem_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(work_sem_));
    if (decode_done_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(decode_done_));
    if (img_buf_) heap_caps_free(img_buf_);
}

void ADBScreenshotScreen::onExit() { stop_engine(); }

void ADBScreenshotScreen::stop_engine() {
    if (stream_) {
        stream_->close();
        stream_.reset();
    }
    if (decode_task_) {
        decode_stop_.store(true);
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(work_sem_));  // wake to exit
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(decode_done_), portMAX_DELAY);
        decode_task_ = nullptr;  // sems freed in the dtor (a late on_stream_close
                                 // may still give work_sem_ while the screen is alive)
    }
}

void ADBScreenshotScreen::build() {
    lv_obj_t* content = nullptr;
    app::preview_chrome(this, "Screenshot", &content);

    // The stage fills the space above the button row and centers whatever it
    // holds (spinner while capturing, the decoded image, or an error label).
    stage_ = lv_obj_create(content);
    lv_obj_remove_style_all(stage_);
    lv_obj_set_width(stage_, LV_PCT(100));
    lv_obj_set_flex_grow(stage_, 1);
    lv_obj_remove_flag(stage_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(stage_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(stage_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(stage_, 16, 0);

    buttons_ = lv_obj_create(content);
    lv_obj_remove_style_all(buttons_);
    lv_obj_set_size(buttons_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(buttons_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(buttons_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(buttons_, 24, 0);

    auto make_action = [this](const char* icon, const char* text, lv_obj_t** icon_out,
                              lv_obj_t** label_out,
                              std::function<void(lv_event_t*)> cb) {
        auto b = lv_button_create(buttons_);
        lv_obj_remove_style_all(b);
        lv_obj_set_height(b, 88);
        lv_obj_set_flex_grow(b, 1);  // the two buttons share the row evenly
        lv_obj_set_style_bg_color(b, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x444444), 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(b, 2, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(b, 12, 0);
        lv_obj_add_event_fn(b, LV_EVENT_CLICKED, std::move(cb));

        auto il = lv_label_create(b);
        lv_label_set_text(il, icon);
        lv_obj_set_style_text_font(il, R.font.lucide_40, 0);
        auto tl = lv_label_create(b);
        lv_label_set_text(tl, text);
        lv_obj_set_style_text_font(tl, &lv_font_montserrat_28, 0);
        if (icon_out) *icon_out = il;
        if (label_out) *label_out = tl;
        return b;
    };

    save_btn_ = make_action(LUCIDE_HARD_DRIVE_DOWNLOAD, "Save to SD", &save_icon_,
                            &save_label_, [this](lv_event_t*) { save_to_sd(); });
    make_action(LUCIDE_REFRESH_CW, "Re-capture", nullptr, nullptr,
                [this](lv_event_t*) { start_capture(); });

    start_capture();
}

void ADBScreenshotScreen::show_capturing() {
    image_ = nullptr;
    lv_obj_clean(stage_);
    auto spinner = lv_spinner_create(stage_);
    lv_obj_set_size(spinner, 80, 80);
    auto label = lv_label_create(stage_);
    lv_label_set_text(label, "Capturing...");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x888888), 0);
    lv_obj_add_flag(buttons_, LV_OBJ_FLAG_HIDDEN);
}

void ADBScreenshotScreen::start_capture() {
    if (capturing_) return;  // a capture is already in flight
    show_capturing();
    have_shot_ = false;

    if (!decode_task_) {
        work_sem_ = xSemaphoreCreateBinary();
        decode_done_ = xSemaphoreCreateBinary();
        TaskHandle_t t = nullptr;
        // Core 1, priority 3 (below the prio-5 adb reader / LVGL) so the heavy PNG
        // inflate+downscale never preempts them.
        xTaskCreatePinnedToCore(&ADBScreenshotScreen::decode_trampoline, "scshot_dec",
                                8192, this, 3, &t, 1);
        decode_task_ = t;
    }

    auto* client = app::adb_client();
    if (!client || client->state() != adb::ConnectionState::Online || !img_buf_) {
        present(false);
        return;
    }
    png_.clear();
    // exec: (not shell:) — binary-safe, no PTY CR/LF translation of the image bytes.
    auto self = std::static_pointer_cast<ADBScreenshotScreen>(shared_from_this());
    stream_ = client->open_stream("exec:screencap -p",
                                  std::weak_ptr<adb::StreamListener>(self));
    if (!stream_) {
        present(false);
        return;
    }
    capturing_ = true;
}

void ADBScreenshotScreen::present(bool ok) {
    capturing_ = false;
    if (exited()) return;
    lv_obj_clean(stage_);
    image_ = nullptr;
    have_shot_ = ok;

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
    } else {
        adb::Client* client = app::adb_client();
        bool online = client && client->state() == adb::ConnectionState::Online;
        auto label = lv_label_create(stage_);
        lv_label_set_text(label, online ? "Capture failed." : "Not connected.");
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x888888), 0);
    }

    // Save is only meaningful with a captured image — grey it otherwise.
    lv_color_t fg = have_shot_ ? lv_color_black() : lv_color_hex(0xb0b0b0);
    lv_obj_set_style_text_color(save_icon_, fg, 0);
    lv_obj_set_style_text_color(save_label_, fg, 0);
    lv_obj_set_style_border_color(save_btn_,
                                  have_shot_ ? lv_color_hex(0x444444) : lv_color_hex(0xb0b0b0), 0);
    lv_obj_remove_flag(buttons_, LV_OBJ_FLAG_HIDDEN);
}

void ADBScreenshotScreen::on_stream_data(adb::Stream*, const uint8_t* data, size_t len) {
    png_.insert(png_.end(), data, data + len);
}

void ADBScreenshotScreen::on_stream_close(adb::Stream*, adb::Error) {
    // Reader thread: hand the captured PNG to the decode task and return —
    // decode/downscale must not run here or it blocks the reader (and thus LVGL).
    if (work_sem_) xSemaphoreGive(static_cast<SemaphoreHandle_t>(work_sem_));
}

void ADBScreenshotScreen::decode_trampoline(void* arg) {
    static_cast<ADBScreenshotScreen*>(arg)->decode_loop();
}

void ADBScreenshotScreen::decode_loop() {
    for (;;) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(work_sem_), portMAX_DELAY);
        if (decode_stop_.load()) break;

        const uint8_t* d = png_.data();
        size_t n = png_.size();
        bool is_png = n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
        bool ok = is_png && app::decode_png_downscale_rgb565(
                                d, n, reinterpret_cast<uint16_t*>(img_buf_), kMaxW,
                                kMaxH, &frame_w_, &frame_h_, &src_w_, &src_h_);
        if (ok)
            ESP_LOGI(TAG, "screenshot %dx%d -> %dx%d %zukB", src_w_, src_h_, frame_w_,
                     frame_h_, n / 1024);
        else
            ESP_LOGW(TAG, "screenshot decode failed (%zu B)", n);
        // Keep png_ — Save writes the full-resolution PNG; the next capture clears it.

        // weak, not shared_from_this(): an adb disconnect could give us work while
        // the screen is already in its dtor (stop_engine joins this task there),
        // where shared_from_this() throws. The lock runs on the LVGL thread,
        // serialized with the dtor, so an expired weak just skips a dead screen.
        lv_async_call([weak = weak_from_this(), ok]() {
            auto base = weak.lock();
            if (!base) return;
            auto self = std::static_pointer_cast<ADBScreenshotScreen>(base);
            if (self->exited()) return;
            self->stream_.reset();
            self->present(ok);
        });
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(decode_done_));
    vTaskDelete(nullptr);
}

void ADBScreenshotScreen::save_to_sd() {
    if (save_card_) return;  // a save is already in flight
    if (!have_shot_ || png_.empty()) {
        app::modal_message(root_, "Save", "No screenshot to save.");
        return;
    }
    if (!bsp_sd_is_mounted()) {
        esp_err_t err = bsp_sd_mount(kMountPoint, NULL);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            app::modal_message(root_, "Save failed", "SD card not found.");
            return;
        }
    }
    // screenshot_YYYYMMDD_HHMMSS.png once the clock is synced from the phone, else
    // the RTC-less screenshot_NNN.png fallback.
    std::string path = app::sysclock::dated_path(kMountPoint, "screenshot", "png");
    if (path.empty()) {
        app::modal_message(root_, "Save failed", "Too many screenshots.");
        return;
    }

    // Snapshot the PNG so the writer task owns a stable buffer even if a
    // Re-capture clears png_ mid-write.
    auto job = std::make_shared<ShotSaveJob>();
    job->buf = (uint8_t*)heap_caps_malloc(png_.size(),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!job->buf) {
        app::modal_message(root_, "Save failed", "Out of memory.");
        return;
    }
    memcpy(job->buf, png_.data(), png_.size());
    job->len = png_.size();
    job->path = path;

    save_card_ = app::modal_open(root_);
    auto title = lv_label_create(save_card_);
    lv_label_set_text(title, "Saving screenshot");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    auto text = lv_label_create(save_card_);
    lv_label_set_text(text, path.c_str());
    lv_obj_set_style_text_font(text, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(text, lv_color_hex(0x444444), 0);
    auto spinner = lv_spinner_create(save_card_);
    lv_obj_set_size(spinner, 64, 64);

    // One-shot writer task: a multi-MB write would stall the LVGL thread.
    auto* fn = new std::function<void()>([self = shared_from_this(), this, job]() {
        int fd = ::open(job->path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        bool ok = fd >= 0;
        size_t off = 0;
        while (ok && off < job->len) {
            size_t n = std::min(job->len - off, kWriteChunk);
            ssize_t w = ::write(fd, job->buf + off, n);
            if (w <= 0) {
                ok = false;
                break;
            }
            off += (size_t)w;
        }
        if (fd >= 0) ::close(fd);
        lv_async_call([self, this, job, ok]() {
            if (exited()) return;
            if (save_card_) {
                app::modal_close(save_card_);
                save_card_ = nullptr;
            }
            if (ok) {
                app::modal_message(root_, "Save", (job->path + " saved.").c_str());
            } else {
                app::modal_message(root_, "Save failed",
                                   ("Cannot write " + job->path).c_str());
            }
        });
    });
    BaseType_t created = xTaskCreate(
        [](void* arg) {
            auto* f = static_cast<std::function<void()>*>(arg);
            (*f)();
            delete f;
            vTaskDelete(nullptr);
        },
        "scshot_save", 6144, fn, 2, nullptr);
    if (created != pdPASS) {
        delete fn;  // drops the job (and its buffer) too
        app::modal_close(save_card_);
        save_card_ = nullptr;
        app::modal_message(root_, "Save failed", "Cannot start the writer.");
    }
}
