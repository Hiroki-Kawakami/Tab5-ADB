#include "sd_file_browser_screen.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <utility>

#include "bsp_sd.h"
#include "file_preview.hpp"
#include "screen_manager.hpp"
#include "resources/resources.h"

namespace {

constexpr const char *kMountPoint = "/sd";

}  // namespace

SDFileBrowserScreen::SDFileBrowserScreen(Pick pick)
    : pick_(std::move(pick)), picking_(true) {}

SDFileBrowserScreen::SDFileBrowserScreen(PickDir pick)
    : pick_dir_(std::move(pick)), picking_dir_(true) {}

std::string SDFileBrowserScreen::current_path() const {
    std::string p = kMountPoint;
    for (const auto &name : path_stack_) {
        p += '/';
        p += name;
    }
    return p;
}

bool SDFileBrowserScreen::pickable(const Entry &e) const {
    if (!picking_ || e.dir) return false;
    const std::string &ext = pick_.ext;
    if (ext.empty()) return true;
    if (e.name.size() < ext.size()) return false;
    size_t off = e.name.size() - ext.size();
    for (size_t i = 0; i < ext.size(); ++i) {
        if (std::tolower((unsigned char)e.name[off + i]) !=
            std::tolower((unsigned char)ext[i])) return false;
    }
    return true;
}

void SDFileBrowserScreen::build() {
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root_, lv_color_white(), 0);
    lv_obj_set_style_pad_row(root_, 0, 0);

    auto navigation = lv_obj_create(root_);
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
    lv_obj_add_event_fn(back, LV_EVENT_CLICKED, [this](lv_event_t*){ this->back(); });
    lv_obj_set_style_bg_color(back, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 12, 0);
    auto back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_38, 0);
    lv_obj_set_style_pad_all(back_label, 8, 0);
    title_label_ = lv_label_create(back);
    lv_obj_set_style_text_font(title_label_, &lv_font_montserrat_38, 0);

    auto pad = lv_obj_create(navigation);
    lv_obj_remove_style_all(pad);
    lv_obj_set_flex_grow(pad, 1);

    if (picking_dir_) {
        auto confirm = lv_button_create(navigation);
        lv_obj_set_height(confirm, 72);
        lv_obj_set_style_radius(confirm, 12, 0);
        lv_obj_add_event_fn(confirm, LV_EVENT_CLICKED, [this](lv_event_t*){
            // pop() destroys this screen (and this lambda's storage): copy to
            // locals first, and touch nothing after the pop.
            auto cb = pick_dir_.on_pick;
            std::string dir = current_path();
            screen_manager.pop();
            if (cb) cb(dir);
        });
        auto confirm_label = lv_label_create(confirm);
        lv_label_set_text(confirm_label, pick_dir_.label.c_str());
        lv_obj_set_style_text_font(confirm_label, &lv_font_montserrat_28, 0);
        lv_obj_center(confirm_label);
    }

    auto refresh_button = lv_button_create(navigation);
    lv_obj_remove_style_all(refresh_button);
    lv_obj_set_style_pad_all(refresh_button, 16, 0);
    lv_obj_add_event_fn(refresh_button, LV_EVENT_CLICKED, [this](lv_event_t*){
        load();
        rebuild();
    });
    lv_obj_set_style_bg_color(refresh_button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(refresh_button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(refresh_button, 12, 0);
    auto refresh_icon = lv_label_create(refresh_button);
    lv_label_set_text(refresh_icon, LUCIDE_REFRESH_CW);
    lv_obj_set_style_text_font(refresh_icon, R.font.lucide_40, 0);
    lv_obj_center(refresh_icon);

    list_ = lv_obj_create(root_);
    lv_obj_remove_style_all(list_);
    lv_obj_set_width(list_, LV_PCT(100));
    lv_obj_set_flex_grow(list_, 1);
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    load();
    rebuild();
}

void SDFileBrowserScreen::open(const std::string &name) {
    path_stack_.push_back(name);
    load();
    rebuild();
}

void SDFileBrowserScreen::back() {
    if (!path_stack_.empty()) {
        path_stack_.pop_back();
        load();
        rebuild();
    } else {
        screen_manager.pop();
    }
}

void SDFileBrowserScreen::load() {
    entries_.clear();
    error_.clear();

    if (!bsp_sd_is_mounted()) {
        esp_err_t err = bsp_sd_mount(kMountPoint, NULL);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            error_ = "SD card not found.";
            return;
        }
    }

    std::string path = current_path();
    DIR *dir = opendir(path.c_str());
    if (!dir) {
        error_ = "Cannot open " + path;
        // The card may have been pulled: unmount so the next Refresh retries
        // the mount instead of reusing a dead filesystem.
        if (path_stack_.empty()) bsp_sd_unmount();
        return;
    }
    while (auto *ent = readdir(dir)) {
        if (ent->d_name[0] == '.') continue;
        bool is_dir;
        if (ent->d_type == DT_DIR) {
            is_dir = true;
        } else if (ent->d_type == DT_REG) {
            is_dir = false;
        } else {
            struct stat st;
            std::string full = path + "/" + ent->d_name;
            if (stat(full.c_str(), &st) != 0) continue;
            is_dir = S_ISDIR(st.st_mode);
        }
        entries_.push_back(Entry{ent->d_name, is_dir});
    }
    closedir(dir);
    std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
        if (a.dir != b.dir) return a.dir;  // folders first
        const std::string &x = a.name, &y = b.name;  // case-insensitive name compare
        for (size_t i = 0; i < x.size() && i < y.size(); ++i) {
            int cx = std::tolower((unsigned char)x[i]);
            int cy = std::tolower((unsigned char)y[i]);
            if (cx != cy) return cx < cy;
        }
        return x.size() < y.size();
    });
}

void SDFileBrowserScreen::rebuild() {
    lv_obj_clean(list_);

    lv_label_set_text(title_label_,
                      path_stack_.empty() ? "SD Card" : path_stack_.back().c_str());

    if (!error_.empty()) {
        auto label = lv_label_create(list_);
        lv_label_set_text(label, error_.c_str());
        lv_obj_set_style_margin_ver(label, 80, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x444444), 0);
        return;
    } else if (entries_.empty()) {
        auto label = lv_label_create(list_);
        lv_label_set_text(label, "The folder is empty.");
        lv_obj_set_style_margin_ver(label, 80, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x444444), 0);
    }

    for (const auto &e : entries_) {
        auto button = lv_button_create(list_);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, LV_PCT(100), 80);
        lv_obj_set_style_pad_hor(button, 24, 0);
        lv_obj_set_style_pad_column(button, 24, 0);
        lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (e.dir) {
            lv_obj_add_event_fn(button, LV_EVENT_CLICKED, [this, name = e.name](lv_event_t*){
                open(name);
            });
            lv_obj_set_style_bg_color(button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
        } else if (pickable(e)) {
            std::string path = current_path() + "/" + e.name;
            lv_obj_add_event_fn(button, LV_EVENT_CLICKED, [this, path](lv_event_t*){
                // pop() destroys this screen (and this lambda's storage):
                // copy to locals first, and touch nothing after the pop.
                auto cb = pick_.on_pick;
                std::string p = path;
                screen_manager.pop();
                if (cb) cb(p);
            });
            lv_obj_set_style_bg_color(button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
        } else if (!picking_ && !picking_dir_) {
            // Browse mode: a plain file opens its preview (size/mtime are
            // stat'd by the preview itself — the local FS is fast).
            app::FileRef ref{app::FileRef::Where::SD, current_path() + "/" + e.name};
            lv_obj_add_event_fn(button, LV_EVENT_CLICKED, [ref](lv_event_t*){
                screen_manager.push(app::make_file_preview(ref));
            });
            lv_obj_set_style_bg_color(button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
        }

        auto icon = lv_label_create(button);
        lv_obj_set_width(icon, 48);
        lv_label_set_text(icon, e.dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
        auto label = lv_label_create(button);
        lv_label_set_text(label, e.name.c_str());
        if (!e.dir && ((picking_ && !pickable(e)) || picking_dir_)) {
            // Non-actionable files stay listed for orientation but greyed out.
            lv_obj_set_style_text_color(icon, lv_color_hex(0xb0b0b0), 0);
            lv_obj_set_style_text_color(label, lv_color_hex(0xb0b0b0), 0);
        }

        auto sep = lv_obj_create(list_);
        lv_obj_remove_style_all(sep);
        lv_obj_set_style_bg_color(sep, lv_color_hex(0xc0c0c0), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        lv_obj_set_size(sep, LV_PCT(100), 1);
        lv_obj_set_style_margin_hor(sep, 24, 0);
    }
}
