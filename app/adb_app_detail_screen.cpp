#include "adb_app_detail_screen.hpp"

#include <memory>
#include <utility>

#include "adb.hpp"
#include "adb_app.hpp"
#include "modal.hpp"
#include "screen_manager.hpp"
#include "resources/resources.h"

namespace {

// First "<key>=" occurrence in the dumpsys output, value to end of line
// (install times contain spaces). stop_at_space cuts at the first space for
// fields that share a line with others (versionCode=N minSdkVersion=...).
std::string dump_value(const std::string &out, const std::string &key,
                       bool stop_at_space = false) {
    std::string needle = key + "=";
    size_t pos = out.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    size_t end = out.find('\n', pos);
    if (end == std::string::npos) end = out.size();
    if (stop_at_space) {
        size_t sp = out.find(' ', pos);
        if (sp != std::string::npos && sp < end) end = sp;
    }
    std::string v = out.substr(pos, end - pos);
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == '\n' || s[i] == '\r' || s[i] == ' ')) ++i;
    return s.substr(i);
}

}  // namespace

ADBAppDetailScreen::ADBAppDetailScreen(std::string pkg, bool system_app, bool disabled)
    : pkg_(std::move(pkg)), system_app_(system_app), disabled_(disabled) {}

void ADBAppDetailScreen::build() {
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0xeeeeee), 0);
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
    lv_label_set_text(title, "App Info");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_38, 0);

    content_ = lv_obj_create(root_);
    lv_obj_remove_style_all(content_);
    lv_obj_set_width(content_, LV_PCT(100));
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content_, 24, 0);
    lv_obj_set_style_pad_row(content_, 24, 0);

    // ---- package header ----
    auto header = lv_obj_create(content_);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 24, 0);
    auto icon = lv_label_create(header);
    lv_label_set_text(icon, LUCIDE_PACKAGE);
    lv_obj_set_style_text_font(icon, R.font.lucide_40, 0);
    auto name = lv_label_create(header);
    lv_label_set_text(name, pkg_.c_str());
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);

    // ---- info card (filled by dumpsys) ----
    info_box_ = lv_obj_create(content_);
    lv_obj_set_size(info_box_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(info_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(info_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info_box_, 16, 0);

    // ---- actions ----
    actions_box_ = lv_obj_create(content_);
    lv_obj_remove_style_all(actions_box_);
    lv_obj_set_size(actions_box_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(actions_box_, 16, 0);

    rebuild_info();
    rebuild_actions();
    load_info();
}

void ADBAppDetailScreen::load_info() {
    adb::Client *client = app::adb_client();
    if (!client) return;
    client->exec("dumpsys package " + pkg_, [self = shared_from_this(), this](
                                                adb::Error err, const std::string &out) {
        if (err != adb::Error::Ok) return;  // keep the placeholder dashes
        auto box = std::make_shared<std::string>(out);
        lv_async_call([self, this, box]() {
            if (exited()) return;
            version_   = dump_value(*box, "versionName");
            installed_ = dump_value(*box, "firstInstallTime");
            updated_   = dump_value(*box, "lastUpdateTime");
            code_path_ = dump_value(*box, "codePath");
            info_loaded_ = true;
            rebuild_info();
        });
    });
}

void ADBAppDetailScreen::rebuild_info() {
    lv_obj_clean(info_box_);
    auto row = [this](const char *key, const std::string &value) {
        auto box = lv_obj_create(info_box_);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box, 4, 0);
        auto k = lv_label_create(box);
        lv_label_set_text(k, key);
        lv_obj_set_style_text_font(k, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(k, lv_color_hex(0x888888), 0);
        auto v = lv_label_create(box);
        lv_label_set_text(v, value.empty() ? "-" : value.c_str());
        lv_obj_set_width(v, LV_PCT(100));
        lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
    };
    if (!info_loaded_) {
        auto spinner = lv_spinner_create(info_box_);
        lv_obj_set_size(spinner, 48, 48);
        return;
    }
    row("Version", version_);
    row("Installed", installed_);
    row("Updated", updated_);
    row("Path", code_path_);
    row("Status", disabled_ ? "Disabled" : "Enabled");
}

void ADBAppDetailScreen::rebuild_actions() {
    lv_obj_clean(actions_box_);
    auto action = [this](const char *icon, const char *text, bool destructive,
                         std::function<void(lv_event_t*)> cb) {
        auto button = lv_button_create(actions_box_);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, LV_PCT(100), 80);
        lv_obj_set_style_bg_color(button, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0x444444), 0);
        lv_obj_set_style_radius(button, 12, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(button, 2, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(button, 32, 0);
        lv_obj_set_style_pad_column(button, 24, 0);
        lv_obj_add_event_fn(button, LV_EVENT_CLICKED, std::move(cb));

        auto icon_label = lv_label_create(button);
        lv_label_set_text(icon_label, icon);
        lv_obj_set_style_text_font(icon_label, R.font.lucide_40, 0);
        auto text_label = lv_label_create(button);
        lv_label_set_text(text_label, text);
        lv_obj_set_style_text_font(text_label, &lv_font_montserrat_28, 0);
        if (destructive) {
            lv_obj_set_style_text_color(icon_label, lv_color_hex(0xd32f2f), 0);
            lv_obj_set_style_text_color(text_label, lv_color_hex(0xd32f2f), 0);
        }
    };

    action(LUCIDE_PLAY, "Launch", false, [this](lv_event_t*) {
        run_action("monkey -p " + pkg_ + " -c android.intent.category.LAUNCHER 1",
                   [this](bool ok, std::string out) {
            if (!ok || out.find("Events injected") == std::string::npos) {
                app::modal_message(root_, "Launch failed", out.c_str());
            }
        });
    });
    action(LUCIDE_CIRCLE_STOP, "Force stop", false, [this](lv_event_t*) {
        run_action("am force-stop " + pkg_, [](bool, std::string) {});
    });
    action(LUCIDE_ERASER, "Clear data", true, [this](lv_event_t*) {
        app::modal_confirm(root_, "Clear data",
                           ("All data of " + pkg_ + " will be deleted.").c_str(),
                           "Clear", true, [this]() {
            run_action("pm clear " + pkg_, [this](bool ok, std::string out) {
                if (!ok || out.find("Success") == std::string::npos) {
                    app::modal_message(root_, "Clear data failed", out.c_str());
                }
            });
        });
    });
    if (disabled_) {
        action(LUCIDE_CHECK, "Enable", false, [this](lv_event_t*) {
            run_action("pm enable " + pkg_, [this](bool ok, std::string out) {
                if (ok && out.find("enabled") != std::string::npos) {
                    disabled_ = false;
                    rebuild_info();
                    rebuild_actions();
                } else {
                    app::modal_message(root_, "Enable failed", out.c_str());
                }
            });
        });
    } else if (system_app_) {
        action(LUCIDE_BAN, "Disable", true, [this](lv_event_t*) {
            app::modal_confirm(root_, "Disable app",
                               (pkg_ + " will be disabled for the current user.").c_str(),
                               "Disable", true, [this]() {
                run_action("pm disable-user --user 0 " + pkg_,
                           [this](bool ok, std::string out) {
                    if (ok && out.find("disabled") != std::string::npos) {
                        disabled_ = true;
                        rebuild_info();
                        rebuild_actions();
                    } else {
                        app::modal_message(root_, "Disable failed", out.c_str());
                    }
                });
            });
        });
    }
    if (!system_app_) {
        action(LUCIDE_TRASH_2, "Uninstall", true, [this](lv_event_t*) {
            app::modal_confirm(root_, "Uninstall",
                               (pkg_ + " will be uninstalled.").c_str(),
                               "Uninstall", true, [this]() {
                run_action("pm uninstall " + pkg_, [this](bool ok, std::string out) {
                    if (ok && out.find("Success") != std::string::npos) {
                        screen_manager.pop();  // the app list re-lists in onAppear
                    } else {
                        app::modal_message(root_, "Uninstall failed", out.c_str());
                    }
                });
            });
        });
    }
}

void ADBAppDetailScreen::run_action(const std::string &cmd,
                                    std::function<void(bool ok, std::string out)> on_done) {
    adb::Client *client = app::adb_client();
    if (!client) {
        app::modal_message(root_, "Error", "Not connected.");
        return;
    }
    client->exec(cmd, [self = shared_from_this(), this, on_done = std::move(on_done)](
                          adb::Error err, const std::string &out) {
        // Reader thread: marshal to LVGL.
        auto box = std::make_shared<std::string>(out);
        lv_async_call([self, this, on_done, err, box]() {
            if (exited()) return;
            on_done(err == adb::Error::Ok,
                    trimmed(err == adb::Error::Ok ? *box : adb::to_string(err)));
        });
    });
}
