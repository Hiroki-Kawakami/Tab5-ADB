#include "wifi_screen.hpp"

#include <memory>

#include "adb_app.hpp"
#include "lvgl.hpp"
#include "modal.hpp"
#include "resources/resources.h"
#include "screen_manager.hpp"

namespace {

const char *result_message(wifi::Result r) {
    switch (r) {
        case wifi::Result::ApNotFound:  return "Network not found. It may be out of range.";
        case wifi::Result::AuthFailed:  return "Wrong password, or authentication failed.";
        case wifi::Result::AssocFailed: return "Could not associate with the network.";
        case wifi::Result::IpFailed:    return "Joined the network but could not get an IP address.";
        case wifi::Result::Timeout:     return "The connection timed out.";
        default:                        return "Could not connect to the network.";
    }
}

}  // namespace

void WiFiScreen::build() {
    lv_obj_set_size(root_, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0xeeeeee), 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Navigation bar (back + title + refresh) ----
    lv_obj_t *nav = lv_obj_create(root_);
    lv_obj_remove_style_all(nav);
    lv_obj_set_size(nav, LV_PCT(100), 120);
    lv_obj_set_style_bg_color(nav, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, 0);
    lv_obj_remove_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(nav, 1, 0);
    lv_obj_set_style_border_color(nav, lv_color_hex(0xc0c0c0), 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(nav, 24, 0);
    lv_obj_set_style_pad_column(nav, 16, 0);

    lv_obj_t *back = lv_button_create(nav);
    lv_obj_remove_style_all(back);
    lv_obj_set_flex_flow(back, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(back, 8, 0);
    lv_obj_set_style_pad_column(back, 16, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_add_event_fn(back, LV_EVENT_CLICKED, [](lv_event_t *) { screen_manager.pop(); });
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_38, 0);
    lv_obj_set_style_pad_all(back_label, 8, 0);
    lv_obj_t *title_label = lv_label_create(back);
    lv_label_set_text(title_label, "Wi-Fi");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_38, 0);

    lv_obj_t *nav_spacer = lv_obj_create(nav);
    lv_obj_remove_style_all(nav_spacer);
    lv_obj_set_size(nav_spacer, 1, 1);
    lv_obj_set_flex_grow(nav_spacer, 1);

    lv_obj_t *refresh = lv_button_create(nav);
    lv_obj_remove_style_all(refresh);
    lv_obj_set_style_pad_all(refresh, 12, 0);
    lv_obj_set_style_radius(refresh, 12, 0);
    lv_obj_set_style_bg_color(refresh, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(refresh, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_fn(refresh, LV_EVENT_CLICKED, [this](lv_event_t *) { start_scan(); });
    lv_obj_t *refresh_label = lv_label_create(refresh);
    lv_label_set_text(refresh_label, LUCIDE_REFRESH_CW);
    lv_obj_set_style_text_font(refresh_label, R.font.lucide_40, 0);
    lv_obj_set_style_text_color(refresh_label, lv_color_hex(0x455a64), 0);

    // ---- Body ----
    lv_obj_t *body = lv_obj_create(root_);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 24, 0);
    lv_obj_set_style_pad_row(body, 16, 0);

    // Status line.
    status_label_ = lv_label_create(body);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status_label_, LV_PCT(100));
    lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0x607d8b), 0);
    lv_label_set_text(status_label_, "Wi-Fi off");

    // Scanning spinner (hidden when results are in).
    spinner_ = lv_spinner_create(body);
    lv_obj_set_size(spinner_, 56, 56);
    lv_obj_add_flag(spinner_, LV_OBJ_FLAG_HIDDEN);

    // AP list (scrollable column of row buttons).
    list_ = lv_obj_create(body);
    lv_obj_remove_style_all(list_);
    lv_obj_set_width(list_, LV_PCT(100));
    lv_obj_set_flex_grow(list_, 1);
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_, 8, 0);
    lv_obj_set_style_bg_color(list_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(list_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(list_, 12, 0);
    lv_obj_set_style_pad_all(list_, 8, 0);
}

void WiFiScreen::onEnter() {
    // Register as the persistent listener (held weakly: drops when this screen is
    // freed) and bring the radio up. start() is idempotent.
    std::weak_ptr<wifi::Listener> wl(std::shared_ptr<wifi::Listener>(
        shared_from_this(), static_cast<wifi::Listener *>(this)));
    wifi::manager().start(wl);
    update_status(wifi::manager().status());
}

void WiFiScreen::onAppear() { start_scan(); }

void WiFiScreen::start_scan() {
    if (spinner_) lv_obj_remove_flag(spinner_, LV_OBJ_FLAG_HIDDEN);
    int gen = ++scan_gen_;
    wifi::manager().scan(
        [self = shared_from_this(), this, gen](wifi::Result, std::vector<wifi::AP> aps) {
            lv_async_call([self, this, gen, aps = std::move(aps)]() mutable {
                if (exited() || gen != scan_gen_) return;
                if (spinner_) lv_obj_add_flag(spinner_, LV_OBJ_FLAG_HIDDEN);
                aps_ = std::move(aps);
                rebuild_list();
            });
        });
}

void WiFiScreen::rebuild_list() {
    lv_obj_clean(list_);
    std::string connected = (wifi::manager().status().state == wifi::State::Connected)
                                ? wifi::manager().status().ssid
                                : std::string();
    for (const auto &ap : aps_) {
        bool secured = ap.secured;
        std::string ssid = ap.ssid;

        lv_obj_t *row = lv_button_create(list_);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 72);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xeceff1), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_pad_column(row, 14, 0);
        lv_obj_add_event_fn(row, LV_EVENT_CLICKED,
                            [this, ssid, secured](lv_event_t *) { select_ap(ssid, secured); });

        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, LUCIDE_WIFI);
        lv_obj_set_style_text_font(icon, R.font.lucide_40, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(0x455a64), 0);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, ssid.c_str());
        lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
        lv_obj_set_flex_grow(name, 1);
        if (ssid == connected)
            lv_obj_set_style_text_color(name, lv_color_hex(0x2196f3), 0);

        if (secured) {
            lv_obj_t *lock = lv_label_create(row);
            lv_label_set_text(lock, LUCIDE_LOCK);
            lv_obj_set_style_text_font(lock, R.font.lucide_40, 0);
            lv_obj_set_style_text_color(lock, lv_color_hex(0x90a4ae), 0);
        }
        lv_obj_t *rssi = lv_label_create(row);
        lv_label_set_text_fmt(rssi, "%d", ap.rssi);
        lv_obj_set_style_text_font(rssi, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(rssi, lv_color_hex(0x90a4ae), 0);
    }
}

void WiFiScreen::update_status(const wifi::Status &s) {
    if (!status_label_) return;
    switch (s.state) {
        case wifi::State::Connected:
            lv_label_set_text_fmt(status_label_, "Connected to %s\nIP %s",
                                  s.ssid.c_str(), s.ip.c_str());
            lv_obj_set_style_text_color(status_label_, lv_color_hex(0x2e7d32), 0);
            break;
        case wifi::State::Connecting:
            lv_label_set_text_fmt(status_label_, "Connecting to %s...", s.ssid.c_str());
            lv_obj_set_style_text_color(status_label_, lv_color_hex(0x607d8b), 0);
            break;
        default: {
            std::string saved = wifi::manager().saved_ssid();
            if (!saved.empty())
                lv_label_set_text_fmt(status_label_, "Not connected (saved: %s)", saved.c_str());
            else
                lv_label_set_text(status_label_, "Not connected");
            lv_obj_set_style_text_color(status_label_, lv_color_hex(0x607d8b), 0);
            break;
        }
    }
}

void WiFiScreen::on_wifi_state(const wifi::Status &status) {
    lv_async_call([self = shared_from_this(), this, status]() {
        if (exited()) return;
        update_status(status);
        rebuild_list();  // recolor the connected row
    });
}

void WiFiScreen::select_ap(const std::string &ssid, bool secured) {
    if (secured) open_password_modal(ssid);
    else do_connect(ssid, "");
}

void WiFiScreen::open_password_modal(const std::string &ssid) {
    if (pw_modal_) return;
    pw_modal_ = app::modal_open(root_);

    lv_obj_t *title = lv_label_create(pw_modal_);
    lv_label_set_text_fmt(title, "Connect to %s", ssid.c_str());
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_t *ta = lv_textarea_create(pw_modal_);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_placeholder_text(ta, "Password");
    lv_obj_set_width(ta, LV_PCT(100));  // fit the card content area (was 600 = overflow)
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_20, 0);

    // The keyboard lives on the SCREEN (root_), pinned full-width to the bottom —
    // NOT inside the modal card (where it overflowed). Created after the scrim so
    // it renders above it (taps reach it); root_ is a flex column, so take it out
    // of the layout (IGNORE_LAYOUT) before aligning, like the modal scrim. Tracked
    // separately and torn down by close_password_modal().
    constexpr int kKeyboardH = 420;
    pw_keyboard_ = lv_keyboard_create(root_);
    lv_obj_add_flag(pw_keyboard_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(pw_keyboard_, PANEL_W, kKeyboardH);
    lv_obj_align(pw_keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(pw_keyboard_, ta);
    lv_obj_t *kb = pw_keyboard_;

    lv_obj_t *btn_row = lv_obj_create(pw_modal_);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 16, 0);

    auto make_btn = [btn_row](const char *txt) {
        lv_obj_t *b = lv_button_create(btn_row);
        lv_obj_set_size(b, 160, 64);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_center(l);
        return b;
    };

    // Close + connect are deferred via lv_async_call so the dialog (incl. the
    // lv_keyboard) is deleted OUTSIDE the in-flight button/keyboard event — tearing
    // a keyboard down mid-event dispatch hangs LVGL.
    lv_obj_t *cancel = make_btn("Cancel");
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x90a4ae), 0);
    lv_obj_add_event_fn(cancel, LV_EVENT_CLICKED, [this](lv_event_t *) {
        lv_async_call([self = shared_from_this(), this]() {
            if (exited()) return;
            close_password_modal();
        });
    });

    lv_obj_t *connect = make_btn("Connect");
    auto connect_fn = [this, ta, ssid](lv_event_t *) {
        std::string pass = lv_textarea_get_text(ta);
        lv_async_call([self = shared_from_this(), this, ssid, pass]() {
            if (exited()) return;
            close_password_modal();
            do_connect(ssid, pass);
        });
    };
    lv_obj_add_event_fn(connect, LV_EVENT_CLICKED, connect_fn);
    // The keyboard's own OK (check) key also confirms.
    lv_obj_add_event_fn(kb, LV_EVENT_READY, connect_fn);

    // Center the card vertically in the area ABOVE the keyboard (modal_open
    // centers it over the whole screen, which the keyboard would crowd). Resolve
    // the SIZE_CONTENT card height first, then place it in the [0, top-of-keyboard]
    // band.
    lv_obj_update_layout(pw_modal_);
    int avail = PANEL_H - kKeyboardH;
    int y = (avail - lv_obj_get_height(pw_modal_)) / 2;
    lv_obj_align(pw_modal_, LV_ALIGN_TOP_MID, 0, y);
}

void WiFiScreen::close_password_modal() {
    // The keyboard is a sibling of the modal (on root_), so close both. Deferred
    // by the callers via lv_async_call so the keyboard is torn down outside its
    // own in-flight event (tearing it down mid-event hangs LVGL).
    if (pw_keyboard_) { lv_obj_delete(pw_keyboard_); pw_keyboard_ = nullptr; }
    if (pw_modal_) { app::modal_close(pw_modal_); pw_modal_ = nullptr; }
}

void WiFiScreen::do_connect(const std::string &ssid, const std::string &password) {
    // Connect-progress modal (spinner + message).
    connecting_modal_ = app::modal_open(root_);
    lv_obj_t *header = lv_obj_create(connecting_modal_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 24, 0);
    lv_obj_t *spinner = lv_spinner_create(header);
    lv_obj_set_size(spinner, 64, 64);
    lv_obj_t *msg = lv_label_create(header);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_28, 0);
    lv_label_set_text_fmt(msg, "Connecting to %s...", ssid.c_str());

    wifi::manager().connect(ssid, password,
        [self = shared_from_this(), this](wifi::Result r) {
            lv_async_call([self, this, r]() {
                if (exited()) return;
                if (connecting_modal_) { app::modal_close(connecting_modal_); connecting_modal_ = nullptr; }
                if (r != wifi::Result::Ok)
                    app::modal_message(root_, "Connection failed", result_message(r));
                // Success: on_wifi_state already updated the status card.
            });
        });
}
