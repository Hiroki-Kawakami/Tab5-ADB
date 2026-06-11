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

    list_ = lv_obj_create(root_);
    lv_obj_remove_style_all(list_);
    lv_obj_set_width(list_, LV_PCT(100));
    lv_obj_set_flex_grow(list_, 1);
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
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
        // Reader thread: hand the output to the LVGL thread.
        auto box = std::make_shared<std::string>(out);
        lv_async_call([self, this, gen, err, box]() {
            if (exited() || gen != load_gen_) return;
            loading_ = false;
            if (err != adb::Error::Ok) {
                error_ = std::string("Error: ") + adb::to_string(err);
                rebuild();
                return;
            }
            user_pkgs_.clear();
            system_pkgs_.clear();
            disabled_.clear();
            parse_sections(*box, &user_pkgs_, &system_pkgs_, &disabled_);
            listed_once_ = true;
            rebuild();
        });
    });
}

void ADBAppManagerScreen::rebuild() {
    lv_obj_clean(list_);

    if (filter_ == Filter::User) {
        lv_obj_add_state(user_btn_, LV_STATE_CHECKED);
        lv_obj_remove_state(system_btn_, LV_STATE_CHECKED);
    } else {
        lv_obj_add_state(system_btn_, LV_STATE_CHECKED);
        lv_obj_remove_state(user_btn_, LV_STATE_CHECKED);
    }

    if (loading_ && !listed_once_) {
        auto spinner = lv_spinner_create(list_);
        lv_obj_set_size(spinner, 80, 80);
        lv_obj_set_style_margin_all(spinner, 80, 0);
        return;
    }
    if (!error_.empty()) {
        auto label = lv_label_create(list_);
        lv_label_set_text(label, error_.c_str());
        lv_obj_set_style_margin_ver(label, 80, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x444444), 0);
        return;
    }
    if (listed_once_ && filtered().empty()) {
        auto label = lv_label_create(list_);
        lv_label_set_text(label, "No apps.");
        lv_obj_set_style_margin_ver(label, 80, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x444444), 0);
    }

    for (const auto &pkg : filtered()) {
        bool disabled = disabled_.count(pkg) != 0;

        auto button = lv_button_create(list_);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, LV_PCT(100), 80);
        lv_obj_set_style_pad_hor(button, 24, 0);
        lv_obj_set_style_pad_column(button, 24, 0);
        lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
        bool system_app = filter_ == Filter::System;
        lv_obj_add_event_fn(button, LV_EVENT_CLICKED,
                            [pkg, system_app, disabled](lv_event_t*){
            screen_manager.push(
                std::make_shared<ADBAppDetailScreen>(pkg, system_app, disabled));
        });

        auto icon = lv_label_create(button);
        lv_label_set_text(icon, LUCIDE_PACKAGE);
        lv_obj_set_style_text_font(icon, R.font.lucide_40, 0);
        auto label = lv_label_create(button);
        lv_label_set_text(label, pkg.c_str());
        lv_obj_set_flex_grow(label, 1);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        if (disabled) {
            lv_obj_set_style_text_color(icon, lv_color_hex(0xb0b0b0), 0);
            lv_obj_set_style_text_color(label, lv_color_hex(0xb0b0b0), 0);
            auto tag = lv_label_create(button);
            lv_label_set_text(tag, "disabled");
            lv_obj_set_style_text_color(tag, lv_color_hex(0xb0b0b0), 0);
            lv_obj_set_style_text_font(tag, &lv_font_montserrat_20, 0);
        }

        auto sep = lv_obj_create(list_);
        lv_obj_remove_style_all(sep);
        lv_obj_set_style_bg_color(sep, lv_color_hex(0xc0c0c0), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        lv_obj_set_size(sep, LV_PCT(100), 1);
        lv_obj_set_style_margin_hor(sep, 24, 0);
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
