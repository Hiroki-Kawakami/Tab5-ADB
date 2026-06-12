#include "file_preview.hpp"

#include <sys/stat.h>

#include <cctype>
#include <cstdio>
#include <ctime>
#include <memory>
#include <utility>

#include "adb.hpp"
#include "adb_app.hpp"
#include "adb_file_browser_screen.hpp"
#include "apk_preview_screen.hpp"
#include "file_transfer.hpp"
#include "screen.hpp"
#include "screen_manager.hpp"
#include "sd_file_browser_screen.hpp"
#include "resources/resources.h"

namespace app {

namespace {

std::string lower_ext(const std::string &name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = name.substr(dot);
    for (auto &c : ext) c = (char)std::tolower((unsigned char)c);
    return ext;
}

std::string format_mtime(uint32_t mtime) {
    if (mtime == 0) return "-";
    time_t t = (time_t)mtime;
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

}  // namespace

// ---- building blocks shared by the preview screens --------------------------

void preview_chrome(Screen *screen, const char *title_text, lv_obj_t **content_out) {
    lv_obj_t *root = screen->root_;
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root, lv_color_hex(0xeeeeee), 0);
    lv_obj_set_style_pad_row(root, 0, 0);

    auto navigation = lv_obj_create(root);
    lv_obj_remove_style_all(navigation);
    lv_obj_set_size(navigation, LV_PCT(100), 120);
    lv_obj_set_style_bg_color(navigation, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(navigation, LV_OPA_COVER, 0);
    lv_obj_remove_flag(navigation, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(navigation, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(navigation, 1, 0);
    lv_obj_set_style_border_color(navigation, lv_color_hex(0xc0c0c0), 0);
    lv_obj_set_flex_flow(navigation, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(navigation, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(navigation, 24, 0);
    lv_obj_set_style_pad_column(navigation, 24, 0);

    auto back = lv_button_create(navigation);
    lv_obj_remove_style_all(back);
    lv_obj_set_flex_flow(back, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(back, 8, 0);
    lv_obj_set_style_pad_column(back, 16, 0);
    lv_obj_add_event_fn(back, LV_EVENT_CLICKED, [](lv_event_t*){ screen_manager.pop(); });
    lv_obj_set_style_bg_color(back, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 12, 0);
    auto back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_38, 0);
    lv_obj_set_style_pad_all(back_label, 8, 0);
    auto title = lv_label_create(back);
    lv_label_set_text(title, title_text);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_38, 0);

    auto content = lv_obj_create(root);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, 24, 0);
    lv_obj_set_style_pad_row(content, 24, 0);
    *content_out = content;
}

void preview_header(lv_obj_t *content, const char *icon_glyph, const std::string &name) {
    auto header = lv_obj_create(content);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 24, 0);
    auto icon = lv_label_create(header);
    lv_label_set_text(icon, icon_glyph);
    lv_obj_set_style_text_font(icon, R.font.lucide_40, 0);
    auto label = lv_label_create(header);
    lv_label_set_text(label, name.c_str());
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
}

void preview_info_row(lv_obj_t *box, const char *key, const std::string &value) {
    auto row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row, 4, 0);
    auto k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_font(k, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(k, lv_color_hex(0x888888), 0);
    auto v = lv_label_create(row);
    lv_label_set_text(v, value.empty() ? "-" : value.c_str());
    lv_obj_set_width(v, LV_PCT(100));
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
}

void preview_action(lv_obj_t *box, const char *icon_glyph, const char *text,
                    bool enabled, std::function<void(lv_event_t *)> cb) {
    auto button = lv_button_create(box);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, LV_PCT(100), 80);
    lv_obj_set_style_bg_color(button, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(enabled ? 0x444444 : 0xb0b0b0), 0);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(button, 32, 0);
    lv_obj_set_style_pad_column(button, 24, 0);
    if (enabled) {
        lv_obj_set_style_bg_color(button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(button, 2, LV_STATE_PRESSED);
        lv_obj_add_event_fn(button, LV_EVENT_CLICKED, std::move(cb));
    }

    auto icon_label = lv_label_create(button);
    lv_label_set_text(icon_label, icon_glyph);
    lv_obj_set_style_text_font(icon_label, R.font.lucide_40, 0);
    auto text_label = lv_label_create(button);
    lv_label_set_text(text_label, text);
    lv_obj_set_style_text_font(text_label, &lv_font_montserrat_28, 0);
    if (!enabled) {
        lv_obj_set_style_text_color(icon_label, lv_color_hex(0xb0b0b0), 0);
        lv_obj_set_style_text_color(text_label, lv_color_hex(0xb0b0b0), 0);
    }
}

namespace {

// Generic preview: file info card + the copy actions (file_transfer jobs).
// The fallback for every extension without a dedicated screen.
class GenericFilePreviewScreen : public Screen {
public:
    explicit GenericFilePreviewScreen(FileRef ref) : ref_(std::move(ref)) {}

    void build() override;
    void onExit() override {
        // The progress modal dies with this screen; the job notices and ends
        // quietly once the transfer sees the abort.
        if (job_) job_->abort();
    }

private:
    FileRef ref_;
    std::shared_ptr<TransferJob> job_;
    lv_obj_t *info_box_{nullptr};
    lv_obj_t *actions_box_{nullptr};

    void rebuild_info();
    void rebuild_actions();
    void copy_to_sd();
    void copy_to_android();
};

bool adb_online() {
    adb::Client *c = app::adb_client();
    return c && c->state() == adb::ConnectionState::Online;
}

void GenericFilePreviewScreen::build() {
    lv_obj_t *content = nullptr;
    preview_chrome(this, "File", &content);

    // The SD listing carries no metadata — stat the local file here (fast,
    // local FS). The Android side passes the DirEntry's size/mtime in.
    if (ref_.where == FileRef::Where::SD) {
        struct stat st = {};
        if (stat(ref_.path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            ref_.size = (uint32_t)st.st_size;
            ref_.mtime = (uint32_t)st.st_mtime;
        }
    }

    preview_header(content, LUCIDE_FILE, ref_.name());

    info_box_ = lv_obj_create(content);
    lv_obj_set_size(info_box_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(info_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(info_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info_box_, 16, 0);

    actions_box_ = lv_obj_create(content);
    lv_obj_remove_style_all(actions_box_);
    lv_obj_set_size(actions_box_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(actions_box_, 16, 0);

    rebuild_info();
    rebuild_actions();
}

void GenericFilePreviewScreen::rebuild_info() {
    lv_obj_clean(info_box_);
    preview_info_row(info_box_, "Location",
                 ref_.where == FileRef::Where::SD ? "Tab5 SD card" : "Android device");
    preview_info_row(info_box_, "Size", format_size(ref_.size));
    preview_info_row(info_box_, "Modified", format_mtime(ref_.mtime));
    preview_info_row(info_box_, "Path", ref_.path);
}

void GenericFilePreviewScreen::rebuild_actions() {
    lv_obj_clean(actions_box_);
    if (ref_.where == FileRef::Where::Android) {
        preview_action(actions_box_, LUCIDE_HARD_DRIVE_DOWNLOAD, "Copy to SD Card",
                          true, [this](lv_event_t *) { copy_to_sd(); });
    } else {
        bool online = adb_online();
        preview_action(actions_box_, LUCIDE_SMARTPHONE,
                          online ? "Copy to Android" : "Copy to Android (not connected)",
                          online, [this](lv_event_t *) { copy_to_android(); });
    }
}

void GenericFilePreviewScreen::copy_to_sd() {
    auto weak = std::weak_ptr<GenericFilePreviewScreen>(
        std::static_pointer_cast<GenericFilePreviewScreen>(shared_from_this()));
    screen_manager.push(std::make_shared<SDFileBrowserScreen>(SDFileBrowserScreen::PickDir{
        "Copy Here",
        [weak](const std::string &dir) {
            // LVGL thread, after the picker popped (this screen is on top again).
            if (auto self = weak.lock(); self && !self->exited()) {
                self->job_ = pull_to_sd(self->root_, self->ref_.path, self->ref_.size,
                                        dir, {});
            }
        }}));
}

void GenericFilePreviewScreen::copy_to_android() {
    auto weak = std::weak_ptr<GenericFilePreviewScreen>(
        std::static_pointer_cast<GenericFilePreviewScreen>(shared_from_this()));
    screen_manager.push(std::make_shared<ADBFileBrowserScreen>(
        "/sdcard", ADBFileBrowserScreen::PickDir{
        "Copy Here",
        [weak](const std::string &dir) {
            if (auto self = weak.lock(); self && !self->exited()) {
                self->job_ = push_to_android(self->root_, self->ref_.path, dir, {});
            }
        }}));
}

}  // namespace

std::string FileRef::name() const {
    size_t pos = path.rfind('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string format_size(uint64_t bytes) {
    char buf[32];
    if (bytes < 1024) {
        snprintf(buf, sizeof(buf), "%u B", (unsigned)bytes);
    } else if (bytes < 1024ull * 1024) {
        snprintf(buf, sizeof(buf), "%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024ull * 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / 1048576.0);
    } else {
        snprintf(buf, sizeof(buf), "%.2f GB", (double)bytes / 1073741824.0);
    }
    return buf;
}

std::shared_ptr<Screen> make_file_preview(const FileRef &ref) {
    // Extension-keyed registry. Dedicated screens register here; everything
    // else falls back to the generic info screen. An Android-side .apk stays
    // generic: the APK parser reads local files only (copy it to SD first).
    std::string ext = lower_ext(ref.name());
    if (ext == ".apk" && ref.where == FileRef::Where::SD) {
        return std::make_shared<ApkPreviewScreen>(ref);
    }
    return std::make_shared<GenericFilePreviewScreen>(ref);
}

}  // namespace app
