#include "adb_file_browser_screen.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <utility>

#include "adb_app.hpp"
#include "file_preview.hpp"
#include "screen_manager.hpp"
#include "resources/resources.h"

namespace {

// A directory entry is navigable if it's a directory or a symlink (which may
// point at one — we attempt the listing and surface an error if it isn't).
bool navigable(const adb::DirEntry &e) { return e.is_dir() || e.is_symlink(); }

bool less_entry(const adb::DirEntry &a, const adb::DirEntry &b) {
    bool ad = navigable(a), bd = navigable(b);
    if (ad != bd) return ad;  // folders first
    const std::string &x = a.name, &y = b.name;  // case-insensitive name compare
    for (size_t i = 0; i < x.size() && i < y.size(); ++i) {
        int cx = std::tolower((unsigned char)x[i]);
        int cy = std::tolower((unsigned char)y[i]);
        if (cx != cy) return cx < cy;
    }
    return x.size() < y.size();
}

}  // namespace

ADBFileBrowserScreen::ADBFileBrowserScreen(std::string path) {
    directory_stack_ = {Directory{
        .path = std::move(path),
        .entries = {},
        .loading = true,
        .error = {},
    }};
}

ADBFileBrowserScreen::ADBFileBrowserScreen(std::string path, PickDir pick)
    : ADBFileBrowserScreen(std::move(path)) {
    pick_dir_ = std::move(pick);
    picking_dir_ = true;
}

ADBFileBrowserScreen::~ADBFileBrowserScreen() {
    // Guard a destroy without onExit: close() stops the worker. The session holds
    // the listener weakly, so the weak ref expires as this screen dies.
    if (sync_) {
        sync_->close();
        sync_.reset();
    }
}

void ADBFileBrowserScreen::build() {
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
            std::string dir = current_directory().path;
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
        current_directory().loading = true;
        current_directory().error.clear();
        rebuild();
        load();
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

    // Open the sync session. The session holds the listener weakly: hand it a
    // shared_ptr aliasing this screen's control block (via shared_from_this) so
    // the weak ref expires when the screen is freed — no detach() needed.
    adb::Client *client = app::adb_client();
    std::shared_ptr<adb::SyncListener> self(
        shared_from_this(), static_cast<adb::SyncListener *>(this));
    sync_ = client ? client->open_sync(self) : nullptr;

    rebuild();
    load();
}

void ADBFileBrowserScreen::onExit() {
    // LVGL thread, before destruction (exited() is already set). close() stops the
    // worker; the session's weak listener ref expires when this screen is freed,
    // so no callback outlives `this`. Any update already marshalled sees exited()
    // and skips the freed widgets.
    if (sync_) {
        sync_->close();
        sync_.reset();
    }
}

void ADBFileBrowserScreen::on_sync_close(adb::Sync * /*s*/, adb::Error /*err*/) {
    // Worker thread: marshal a notice to the LVGL thread (guarded against teardown).
    lv_async_call([self = shared_from_this(), this]() {
        if (exited()) return;
        if (current_directory().loading) {
            current_directory().loading = false;
            current_directory().error = "Session closed.";
            rebuild();
        }
    });
}

void ADBFileBrowserScreen::open(const std::string &path) {
    directory_stack_.push_back(Directory{
        .path = path,
        .entries = {},
        .loading = true,
        .error = {},
    });
    ++nav_gen_;
    rebuild();
    load();
}

void ADBFileBrowserScreen::back() {
    if (directory_stack_.size() > 1) {
        directory_stack_.pop_back();
        ++nav_gen_;  // supersede the in-flight listing of the directory we left
        rebuild();
        if (current_directory().loading) load();  // never finished loading: retry
    } else {
        screen_manager.pop();
    }
}

void ADBFileBrowserScreen::load() {
    if (!sync_) {
        current_directory().loading = false;
        current_directory().error = "Not connected.";
        rebuild();
        return;
    }
    uint32_t gen = nav_gen_;
    std::string path = current_directory().path;
    // Strong `self` keeps the screen alive until the body runs on the LVGL thread,
    // where the last ref drops. exited() skips freed widgets after teardown; gen
    // drops a listing a newer navigation superseded (so it lands in current_dir).
    sync_->list(path, [self = shared_from_this(), this, gen](
                          adb::Error err, std::vector<adb::DirEntry> entries) {
        // Worker thread: hand the result to the LVGL thread.
        auto box = std::make_shared<std::vector<adb::DirEntry>>(std::move(entries));
        lv_async_call([self, this, gen, err, box]() {
            if (exited() || gen != nav_gen_) return;
            auto &dir = current_directory();
            dir.loading = false;
            if (err != adb::Error::Ok) {
                dir.error = std::string("Error: ") + adb::to_string(err);
                rebuild();
                return;
            }
            // Drop "." / ".."; folders first, then case-insensitive by name.
            auto &es = *box;
            es.erase(std::remove_if(es.begin(), es.end(),
                                    [](const adb::DirEntry &e) {
                                        return e.name == "." || e.name == "..";
                                    }),
                     es.end());
            std::sort(es.begin(), es.end(), less_entry);
            dir.entries = std::move(es);
            dir.error.clear();
            rebuild();
        });
    });
}

void ADBFileBrowserScreen::rebuild() {
    lv_obj_clean(list_);

    std::string name = current_directory().path;
    if (name == "/sdcard") {
        name = "Internal Storage";
    } else if (name != "/") {
        size_t pos = name.rfind('/');
        if (pos != std::string::npos) name = name.substr(pos + 1);
    }
    lv_label_set_text(title_label_, name.c_str());

    if (current_directory().loading) {
        auto spinner = lv_spinner_create(list_);
        lv_obj_set_size(spinner, 80, 80);
        lv_obj_set_style_margin_all(spinner, 80, 0);
        return;
    } else if (!current_directory().error.empty()) {
        auto label = lv_label_create(list_);
        lv_label_set_text(label, current_directory().error.c_str());
        lv_obj_set_style_margin_ver(label, 80, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x444444), 0);
        return;
    } else if (current_directory().entries.size() == 0) {
        auto label = lv_label_create(list_);
        lv_label_set_text(label, "The folder is empty.");
        lv_obj_set_style_margin_ver(label, 80, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x444444), 0);
    }

    for (const auto &e : current_directory().entries) {
        auto path = current_directory().path;
        if (path != "/") path += "/";
        path += e.name;

        auto button = lv_button_create(list_);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, LV_PCT(100), 80);
        lv_obj_set_style_pad_hor(button, 24, 0);
        lv_obj_set_style_pad_column(button, 24, 0);
        lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (navigable(e)) {
            lv_obj_add_event_fn(button, LV_EVENT_CLICKED, [this, path](lv_event_t*){
                open(path);
            });
            lv_obj_set_style_bg_color(button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
        } else if (e.is_reg() && !picking_dir_) {
            app::FileRef ref{app::FileRef::Where::Android, path, e.size, e.mtime};
            lv_obj_add_event_fn(button, LV_EVENT_CLICKED, [ref](lv_event_t*){
                screen_manager.push(app::make_file_preview(ref));
            });
            lv_obj_set_style_bg_color(button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
        }

        auto icon = lv_label_create(button);
        lv_obj_set_width(icon, 48);
        lv_label_set_text(icon, e.is_dir()      ? LV_SYMBOL_DIRECTORY
                               : e.is_symlink() ? LV_SYMBOL_LOOP
                                                : LV_SYMBOL_FILE);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
        auto label = lv_label_create(button);
        lv_label_set_text(label, e.name.c_str());
        if (picking_dir_ && !navigable(e)) {
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
