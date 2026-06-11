#include "adb_app_manager_screen.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "esp_heap_caps.h"

#include "adb_app.hpp"
#include "adb_app_detail_screen.hpp"
#include "modal.hpp"
#include "sd_file_browser_screen.hpp"
#include "screen_manager.hpp"
#include "resources/resources.h"

namespace {

// One round trip for everything the screen shows: third-party + system +
// disabled package names, separated so one exec output parses into the three
// sets. `pm list packages` emits "package:<name>" lines.
constexpr const char *kListCmd =
    "pm list packages -3 2>/dev/null; echo ---SEP---; "
    "pm list packages -s 2>/dev/null; echo ---SEP---; "
    "pm list packages -d 2>/dev/null";

void parse_sections(const std::string &out, std::vector<std::string> *user,
                    std::vector<std::string> *system, std::set<std::string> *disabled) {
    int section = 0;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t eol = out.find('\n', pos);
        if (eol == std::string::npos) eol = out.size();
        std::string line = out.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line == "---SEP---") {
            ++section;
            continue;
        }
        if (line.rfind("package:", 0) != 0) continue;
        std::string pkg = line.substr(8);
        if (pkg.empty()) continue;
        switch (section) {
            case 0: user->push_back(std::move(pkg)); break;
            case 1: system->push_back(std::move(pkg)); break;
            default: disabled->insert(std::move(pkg)); break;
        }
    }
    std::sort(user->begin(), user->end());
    std::sort(system->begin(), system->end());
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == '\n' || s[i] == '\r' || s[i] == ' ')) ++i;
    return s.substr(i);
}

constexpr const char *kRemoteApk = "/data/local/tmp/tab5adb_install.apk";
constexpr size_t kReadChunk = 16 * 1024;
constexpr int32_t kRowH = 81;  // 80px row + 1px bottom-border separator

std::string fmt_size(size_t bytes) {
    char buf[32];
    if (bytes < 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / 1048576.0);
    }
    return buf;
}

}  // namespace

void ADBAppManagerScreen::build() {
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
    lv_obj_add_event_fn(back, LV_EVENT_CLICKED, [](lv_event_t*){ screen_manager.pop(); });
    lv_obj_set_style_bg_color(back, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 12, 0);
    auto back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_38, 0);
    lv_obj_set_style_pad_all(back_label, 8, 0);
    auto title = lv_label_create(back);
    lv_label_set_text(title, "Apps");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_38, 0);

    auto pad = lv_obj_create(navigation);
    lv_obj_remove_style_all(pad);
    lv_obj_set_flex_grow(pad, 1);

    auto install_button = lv_button_create(navigation);
    lv_obj_remove_style_all(install_button);
    lv_obj_set_style_pad_all(install_button, 16, 0);
    lv_obj_add_event_fn(install_button, LV_EVENT_CLICKED, [this](lv_event_t*){ pick_apk(); });
    lv_obj_set_style_bg_color(install_button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(install_button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(install_button, 12, 0);
    auto install_icon = lv_label_create(install_button);
    lv_label_set_text(install_icon, LUCIDE_PACKAGE_PLUS);
    lv_obj_set_style_text_font(install_icon, R.font.lucide_40, 0);
    lv_obj_center(install_icon);

    auto refresh_button = lv_button_create(navigation);
    lv_obj_remove_style_all(refresh_button);
    lv_obj_set_style_pad_all(refresh_button, 16, 0);
    lv_obj_add_event_fn(refresh_button, LV_EVENT_CLICKED, [this](lv_event_t*){ refresh(); });
    lv_obj_set_style_bg_color(refresh_button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(refresh_button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(refresh_button, 12, 0);
    auto refresh_icon = lv_label_create(refresh_button);
    lv_label_set_text(refresh_icon, LUCIDE_REFRESH_CW);
    lv_obj_set_style_text_font(refresh_icon, R.font.lucide_40, 0);
    lv_obj_center(refresh_icon);

    // ---- User / System filter toggle ----
    auto filter_row = lv_obj_create(root_);
    lv_obj_remove_style_all(filter_row);
    lv_obj_set_size(filter_row, LV_PCT(100), 72);
    lv_obj_remove_flag(filter_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(filter_row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(filter_row, 1, 0);
    lv_obj_set_style_border_color(filter_row, lv_color_hex(0xc0c0c0), 0);
    lv_obj_set_flex_flow(filter_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(filter_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(filter_row, 24, 0);
    lv_obj_set_style_pad_ver(filter_row, 8, 0);
    lv_obj_set_style_pad_column(filter_row, 16, 0);

    auto filter_button = [this, filter_row](const char *text, Filter f) {
        auto button = lv_button_create(filter_row);
        lv_obj_remove_style_all(button);
        lv_obj_set_height(button, LV_PCT(100));
        lv_obj_set_flex_grow(button, 1);
        lv_obj_set_style_radius(button, 12, 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0xc0c0c0), 0);
        lv_obj_set_style_bg_color(button, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        // The active filter is shown with the theme primary color (set in
        // rebuild() via the CHECKED state).
        lv_obj_set_style_bg_color(button, lv_theme_get_color_primary(button), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(button, lv_color_white(), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_add_event_fn(button, LV_EVENT_CLICKED, [this, f](lv_event_t*){ set_filter(f); });
        auto label = lv_label_create(button);
        lv_label_set_text(label, text);
        lv_obj_center(label);
        return button;
    };
    user_btn_ = filter_button("User", Filter::User);
    system_btn_ = filter_button("System", Filter::System);

    // No layout: the recycled rows position themselves (lv_obj_set_pos) and
    // the invisible extent child defines the scroll range.
    list_ = lv_obj_create(root_);
    lv_obj_remove_style_all(list_);
    lv_obj_set_width(list_, LV_PCT(100));
    lv_obj_set_flex_grow(list_, 1);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_add_event_fn(list_, LV_EVENT_SCROLL, [this](lv_event_t*){ update_rows(false); });

    extent_ = lv_obj_create(list_);
    lv_obj_remove_style_all(extent_);
    lv_obj_set_size(extent_, 1, 0);
    lv_obj_set_pos(extent_, 0, 0);
}

void ADBAppManagerScreen::onAppear() {
    // First show and every return from a sub-screen (detail actions / install
    // change the package set, so re-list).
    refresh();
}

void ADBAppManagerScreen::onExit() {
    // Abort an in-flight install: the source sees `abort` (or the closed
    // session) on the worker thread and the push completion's exited() guard
    // skips the UI; the job dtor releases the file/buffer. The progress dialog
    // dies with root_.
    if (progress_timer_) {
        lv_timer_delete(progress_timer_);
        progress_timer_ = nullptr;
    }
    if (job_) {
        job_->abort = true;
        if (job_->sync) job_->sync->close();
        job_.reset();
    }
}

ADBAppManagerScreen::InstallJob::~InstallJob() {
    if (fd >= 0) close(fd);
    if (buf) heap_caps_free(buf);
}

void ADBAppManagerScreen::set_filter(Filter f) {
    if (filter_ == f) return;
    filter_ = f;
    lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);
    rebuild();
}

void ADBAppManagerScreen::refresh() {
    adb::Client *client = app::adb_client();
    if (!client) {
        loading_ = false;
        error_ = "Not connected.";
        rebuild();
        return;
    }
    loading_ = true;
    error_.clear();
    rebuild();
    uint32_t gen = ++load_gen_;
    client->exec(kListCmd, [self = shared_from_this(), this, gen](
                               adb::Error err, const std::string &out) {
        // Reader thread: parse + sort here so the LVGL thread only swaps the
        // result vectors in.
        struct Parsed {
            std::vector<std::string> user, system;
            std::set<std::string> disabled;
        };
        auto box = std::make_shared<Parsed>();
        if (err == adb::Error::Ok) {
            parse_sections(out, &box->user, &box->system, &box->disabled);
        }
        lv_async_call([self, this, gen, err, box]() {
            if (exited() || gen != load_gen_) return;
            loading_ = false;
            if (err != adb::Error::Ok) {
                error_ = std::string("Error: ") + adb::to_string(err);
                rebuild();
                return;
            }
            user_pkgs_ = std::move(box->user);
            system_pkgs_ = std::move(box->system);
            disabled_ = std::move(box->disabled);
            rebuild();
        });
    });
}

void ADBAppManagerScreen::rebuild() {
    if (filter_ == Filter::User) {
        lv_obj_add_state(user_btn_, LV_STATE_CHECKED);
        lv_obj_remove_state(system_btn_, LV_STATE_CHECKED);
    } else {
        lv_obj_add_state(system_btn_, LV_STATE_CHECKED);
        lv_obj_remove_state(user_btn_, LV_STATE_CHECKED);
    }

    if (status_) {
        lv_obj_delete(status_);
        status_ = nullptr;
    }
    auto status_label = [this](const char *text) {
        status_ = lv_label_create(list_);
        lv_label_set_text(status_, text);
        lv_obj_set_style_text_color(status_, lv_color_hex(0x444444), 0);
        lv_obj_align(status_, LV_ALIGN_TOP_MID, 0, 80);
    };

    size_t count = 0;
    if (loading_) {
        status_ = lv_spinner_create(list_);
        lv_obj_set_size(status_, 80, 80);
        lv_obj_align(status_, LV_ALIGN_TOP_MID, 0, 80);
    } else if (!error_.empty()) {
        status_label(error_.c_str());
    } else if (filtered().empty()) {
        status_label("No apps.");
    } else {
        count = filtered().size();
    }

    ensure_pool();
    lv_obj_set_height(extent_, (int32_t)count * kRowH);
    // Keep the scroll position across a re-list (onAppear), clamped when the
    // list shrank below it.
    lv_obj_update_layout(list_);
    int32_t max_scroll = (int32_t)count * kRowH - lv_obj_get_height(list_);
    if (max_scroll < 0) max_scroll = 0;
    if (lv_obj_get_scroll_y(list_) > max_scroll) {
        lv_obj_scroll_to_y(list_, max_scroll, LV_ANIM_OFF);
    }
    update_rows(true);
}

void ADBAppManagerScreen::ensure_pool() {
    if (!pool_.empty()) return;
    lv_obj_update_layout(list_);
    int32_t h = lv_obj_get_height(list_);
    if (h <= 0) h = 1088;  // pre-layout fallback: panel minus nav + filter row
    size_t n = (size_t)(h / kRowH) + 3;  // partial top/bottom + one of lookbehind
    pool_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Row r = {};
        r.btn = lv_button_create(list_);
        lv_obj_remove_style_all(r.btn);
        lv_obj_set_size(r.btn, LV_PCT(100), kRowH);
        lv_obj_add_flag(r.btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_side(r.btn, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(r.btn, 1, 0);
        lv_obj_set_style_border_color(r.btn, lv_color_hex(0xc0c0c0), 0);
        lv_obj_set_style_pad_hor(r.btn, 24, 0);
        lv_obj_set_style_pad_column(r.btn, 24, 0);
        lv_obj_set_flex_flow(r.btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r.btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(r.btn, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r.btn, LV_OPA_COVER, LV_STATE_PRESSED);
        // The handler reads the slot's bound index at tap time (the pool_
        // vector is created once and never reallocates).
        size_t slot = i;
        lv_obj_add_event_fn(r.btn, LV_EVENT_CLICKED, [this, slot](lv_event_t*){
            int idx = pool_[slot].data_idx;
            if (idx < 0 || idx >= (int)filtered().size()) return;
            const std::string &pkg = filtered()[idx];
            screen_manager.push(std::make_shared<ADBAppDetailScreen>(
                pkg, filter_ == Filter::System, disabled_.count(pkg) != 0));
        });

        r.icon = lv_label_create(r.btn);
        lv_label_set_text(r.icon, LUCIDE_PACKAGE);
        lv_obj_set_style_text_font(r.icon, R.font.lucide_40, 0);
        r.name = lv_label_create(r.btn);
        lv_obj_set_flex_grow(r.name, 1);
        lv_label_set_long_mode(r.name, LV_LABEL_LONG_DOT);
        r.tag = lv_label_create(r.btn);
        lv_label_set_text(r.tag, "disabled");
        lv_obj_set_style_text_color(r.tag, lv_color_hex(0xb0b0b0), 0);
        lv_obj_set_style_text_font(r.tag, &lv_font_montserrat_20, 0);
        pool_.push_back(r);
    }
}

void ADBAppManagerScreen::bind_row(Row &r, int idx) {
    if (idx < 0 || idx >= (int)filtered().size()) {
        r.data_idx = -1;
        lv_obj_add_flag(r.btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    r.data_idx = idx;
    lv_obj_remove_flag(r.btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(r.btn, 0, idx * kRowH);
    const std::string &pkg = filtered()[idx];
    bool disabled = disabled_.count(pkg) != 0;
    lv_label_set_text(r.name, pkg.c_str());
    lv_color_t color = disabled ? lv_color_hex(0xb0b0b0) : lv_color_black();
    lv_obj_set_style_text_color(r.icon, color, 0);
    lv_obj_set_style_text_color(r.name, color, 0);
    if (disabled) {
        lv_obj_remove_flag(r.tag, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(r.tag, LV_OBJ_FLAG_HIDDEN);
    }
}

void ADBAppManagerScreen::update_rows(bool force) {
    if (pool_.empty()) return;
    int32_t sy = lv_obj_get_scroll_y(list_);
    int first = (int)(sy / kRowH) - 1;  // one row of lookbehind above the fold
    if (first < 0) first = 0;
    if (!force && first == first_bound_) return;
    first_bound_ = first;
    for (size_t i = 0; i < pool_.size(); ++i) {
        bind_row(pool_[i], first + (int)i);
    }
}

// ---- APK install flow ----

void ADBAppManagerScreen::pick_apk() {
    auto weak = std::weak_ptr<ADBAppManagerScreen>(
        std::static_pointer_cast<ADBAppManagerScreen>(shared_from_this()));
    screen_manager.push(std::make_shared<SDFileBrowserScreen>(SDFileBrowserScreen::Pick{
        ".apk",
        [weak](const std::string &path) {
            // Fires on the LVGL thread after the picker popped (this screen is
            // on top again).
            if (auto self = weak.lock(); self && !self->exited()) {
                self->confirm_install(path);
            }
        }}));
}

void ADBAppManagerScreen::confirm_install(const std::string &path) {
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        app::modal_message(root_, "Install failed", "Cannot read the file.");
        return;
    }
    std::string name = path.substr(path.rfind('/') + 1);
    char text[160];
    snprintf(text, sizeof(text), "%s (%s) will be installed.",
             name.c_str(), fmt_size((size_t)st.st_size).c_str());
    app::modal_confirm(root_, "Install APK", text, "Install", false,
                       [this, path]() { start_install(path); });
}

void ADBAppManagerScreen::start_install(const std::string &path) {
    adb::Client *client = app::adb_client();
    if (!client) {
        app::modal_message(root_, "Install failed", "Not connected.");
        return;
    }

    auto job = std::make_shared<InstallJob>();
    job->fd = open(path.c_str(), O_RDONLY);
    if (job->fd < 0) {
        app::modal_message(root_, "Install failed", "Cannot open the file.");
        return;
    }
    struct stat st = {};
    fstat(job->fd, &st);
    job->total = (size_t)st.st_size;
    job->buf = (uint8_t *)heap_caps_malloc(kReadChunk, MALLOC_CAP_CACHE_ALIGNED);
    if (!job->buf) {
        app::modal_message(root_, "Install failed", "Out of memory.");
        return;
    }
    std::shared_ptr<adb::SyncListener> listener(
        shared_from_this(), static_cast<adb::SyncListener *>(this));
    job->sync = client->open_sync(listener);
    if (!job->sync) {
        app::modal_message(root_, "Install failed", "Not connected.");
        return;
    }
    job_ = job;

    // ---- progress dialog ----
    progress_card_ = app::modal_open(root_);
    auto title = lv_label_create(progress_card_);
    lv_label_set_text(title, "Installing APK");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    progress_label_ = lv_label_create(progress_card_);
    lv_label_set_text(progress_label_, "");
    lv_obj_set_style_text_font(progress_label_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(progress_label_, lv_color_hex(0x444444), 0);
    progress_bar_ = lv_bar_create(progress_card_);
    lv_obj_set_size(progress_bar_, LV_PCT(100), 16);
    lv_bar_set_range(progress_bar_, 0, 100);
    auto cancel = lv_button_create(progress_card_);
    lv_obj_set_height(cancel, 72);
    lv_obj_set_width(cancel, LV_PCT(100));
    lv_obj_set_style_radius(cancel, 12, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_color(cancel, lv_color_black(), 0);
    auto cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
    lv_obj_add_event_fn(cancel, LV_EVENT_CLICKED, [this](lv_event_t*){
        if (job_) job_->abort = true;  // the source aborts the push
    });
    progress_timer_ = lv_timer_create([](lv_timer_t *t) {
        static_cast<ADBAppManagerScreen *>(lv_timer_get_user_data(t))->update_progress();
    }, 200, this);
    update_progress();

    // The source runs on the Sync worker thread: 16 KB cache-aligned read()
    // chunks (the SD fast path), handed out in cap-sized slices.
    adb::SyncSource source = [job](uint8_t *dst, size_t cap) -> int {
        if (job->abort) return -1;
        if (job->buf_off >= job->buf_len) {
            ssize_t n = read(job->fd, job->buf, kReadChunk);
            if (n < 0) return -1;
            if (n == 0) return 0;
            job->buf_len = (size_t)n;
            job->buf_off = 0;
        }
        size_t n = std::min(cap, job->buf_len - job->buf_off);
        memcpy(dst, job->buf + job->buf_off, n);
        job->buf_off += n;
        job->sent += n;
        return (int)n;
    };
    job->sync->push(kRemoteApk, 0644, (uint32_t)st.st_mtime, std::move(source),
                    [self = shared_from_this(), this, job](adb::Error err) {
        // Sync worker thread: marshal to LVGL.
        lv_async_call([self, this, job, err]() {
            if (exited() || job != job_) return;
            if (err != adb::Error::Ok || job->abort) {
                bool aborted = job->abort;
                close_progress();
                app::adb_client()->exec(std::string("rm -f ") + kRemoteApk,
                                        [](adb::Error, const std::string &) {});
                if (!aborted) {
                    app::modal_message(root_, "Install failed",
                                       (std::string("push: ") + adb::to_string(err)).c_str());
                }
                return;
            }
            // The APK landed in /data/local/tmp; hand off to pm install. Stop
            // the byte counter so the label keeps the "Installing..." text.
            lv_timer_delete(progress_timer_);
            progress_timer_ = nullptr;
            lv_bar_set_value(progress_bar_, 100, LV_ANIM_OFF);
            lv_label_set_text(progress_label_, "Installing...");
            run_pm_install();
        });
    });
}

void ADBAppManagerScreen::run_pm_install() {
    app::adb_client()->exec(std::string("pm install -r ") + kRemoteApk,
                            [self = shared_from_this(), this, job = job_](
                                adb::Error err, const std::string &out) {
        // Reader thread: marshal to LVGL.
        auto box = std::make_shared<std::string>(out);
        lv_async_call([self, this, job, err, box]() {
            if (exited() || job != job_) return;
            close_progress();
            app::adb_client()->exec(std::string("rm -f ") + kRemoteApk,
                                    [](adb::Error, const std::string &) {});
            if (err == adb::Error::Ok && box->find("Success") != std::string::npos) {
                refresh();
                app::modal_message(root_, "Install", "Install complete.");
            } else {
                app::modal_message(root_, "Install failed",
                                   trimmed(err == adb::Error::Ok ? *box : adb::to_string(err)).c_str());
            }
        });
    });
}

void ADBAppManagerScreen::update_progress() {
    if (!job_ || !progress_bar_) return;
    size_t sent = job_->sent;
    size_t total = job_->total;
    if (total) lv_bar_set_value(progress_bar_, (int32_t)(sent * 100 / total), LV_ANIM_OFF);
    char text[80];
    snprintf(text, sizeof(text), "%s / %s",
             fmt_size(sent).c_str(), fmt_size(total).c_str());
    lv_label_set_text(progress_label_, text);
}

void ADBAppManagerScreen::close_progress() {
    if (progress_timer_) {
        lv_timer_delete(progress_timer_);
        progress_timer_ = nullptr;
    }
    if (progress_card_) {
        app::modal_close(progress_card_);
        progress_card_ = nullptr;
        progress_bar_ = nullptr;
        progress_label_ = nullptr;
    }
    if (job_) {
        if (job_->sync) job_->sync->close();
        job_.reset();
    }
}
