#include "home_screen.hpp"

#include <functional>
#include <memory>

#include "about_screen.hpp"
#include "adb_app.hpp"
#include "adb_device_screen.hpp"
#include "agent_client.hpp"
#include "app_version.hpp"
#include "modal.hpp"
#include "resources/resources.h"
#include "screen_manager.hpp"
#include "sd_file_browser_screen.hpp"
#include "settings.hpp"
#include "settings_screen.hpp"
#include "tcp_history.hpp"
#include "wifi_manager.hpp"

namespace {

// A transparent flex row used for card header lines (icon + title + extras).
lv_obj_t *make_row(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    return row;
}

lv_obj_t *card_title(lv_obj_t *row, const char *icon, const char *title) {
    lv_obj_t *icon_label = lv_label_create(row);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, R.font.lucide_40, 0);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(0x455a64), 0);

    lv_obj_t *title_label = lv_label_create(row);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x455a64), 0);
    return title_label;
}

lv_obj_t *make_card(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);
    return card;
}

}  // namespace

void HomeScreen::build() {
    lv_obj_set_size(root_, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0xeeeeee), 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    build_hero();
    build_body();
}

void HomeScreen::build_hero() {
    lv_obj_t *hero = lv_obj_create(root_);
    lv_obj_remove_style_all(hero);
    lv_obj_set_size(hero, LV_PCT(100), 260);
    lv_obj_set_style_bg_color(hero, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(hero, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(hero, 40, 0);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(hero, 10, 0);

    lv_obj_t *title = lv_label_create(hero);
    lv_label_set_text(title, "Tab5 ADB");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);

    lv_obj_t *version = lv_label_create(hero);
    lv_label_set_text(version, kAppVersion);
    lv_obj_set_style_text_color(version, lv_color_hex(0x90a4ae), 0);
    lv_obj_set_style_text_font(version, &lv_font_montserrat_16, 0);

    // Subtle brand mark on the right, outside the text column.
    lv_obj_t *mark = lv_label_create(hero);
    lv_obj_add_flag(mark, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_label_set_text(mark, LUCIDE_TABLET_SMARTPHONE);
    lv_obj_set_style_text_font(mark, R.font.lucide_40, 0);
    lv_obj_set_style_text_color(mark, lv_color_hex(0x37474f), 0);
    lv_obj_align(mark, LV_ALIGN_RIGHT_MID, 0, 0);
}

void HomeScreen::build_body() {
    lv_obj_t *body = lv_obj_create(root_);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 24, 0);
    lv_obj_set_style_pad_bottom(body, 22, 0);
    lv_obj_set_style_pad_row(body, 24, 0);

    // ---- USB connection card ----
    lv_obj_t *usb_card = make_card(body);
    card_title(make_row(usb_card), LUCIDE_USB, "USB");

    connect_btn_ = lv_button_create(usb_card);
    lv_obj_set_size(connect_btn_, LV_PCT(100), 100);
    lv_obj_t *btn_label = lv_label_create(connect_btn_);
    lv_label_set_text(btn_label, "Connect");
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_28, 0);
    lv_obj_center(btn_label);
    lv_obj_add_event_fn(connect_btn_, LV_EVENT_CLICKED,
                        [this](lv_event_t *) { start_connect(); });

    // ---- Wireless (TCP/IP) card. The Wi-Fi setup is wired now (gets the Tab5
    // onto a LAN); the phone-IP connect row is still a placeholder — ADB-over-TCP
    // (the embedded_adb TCP transport) lands later, on top of this Wi-Fi link. ----
    lv_obj_t *tcp_card = make_card(body);
    lv_obj_set_flex_grow(tcp_card, 1);
    card_title(make_row(tcp_card), LUCIDE_WIFI, "Wireless (TCP/IP)");

    wifi_status_ = lv_label_create(tcp_card);
    lv_obj_set_style_text_font(wifi_status_, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(wifi_status_, lv_color_hex(0x90a4ae), 0);
    refresh_wifi_status();  // Wi-Fi is set up from Settings; this card just shows status

    lv_obj_t *input_row = make_row(tcp_card);
    tcp_addr_ = lv_textarea_create(input_row);
    lv_textarea_set_one_line(tcp_addr_, true);
    lv_textarea_set_placeholder_text(tcp_addr_, "192.168.1.100:5555");
    lv_obj_set_style_text_font(tcp_addr_, &lv_font_montserrat_20, 0);
    lv_obj_set_height(tcp_addr_, 60);
    // One-line textarea places its label at pad_top, so the default padding leaves
    // the text top-aligned in the fixed 60px box; symmetric vertical padding sized
    // to the line height centers it (60 - ~26 line height) / 2 ≈ 17.
    lv_obj_set_style_pad_ver(tcp_addr_, 17, 0);
    lv_obj_set_flex_grow(tcp_addr_, 1);
    // Focusing the field raises the on-screen keyboard; lv_keyboard drops
    // CLICK_FOCUSABLE, so a re-tap of the already-focused field sends no FOCUSED —
    // hook CLICKED too (the logcat-screen gotcha).
    lv_obj_add_event_fn(tcp_addr_, LV_EVENT_FOCUSED,
                        [this](lv_event_t *) { show_tcp_keyboard(); });
    lv_obj_add_event_fn(tcp_addr_, LV_EVENT_CLICKED,
                        [this](lv_event_t *) { show_tcp_keyboard(); });

    tcp_connect_btn_ = lv_button_create(input_row);
    lv_obj_set_size(tcp_connect_btn_, 160, 60);
    lv_obj_t *tcp_btn_label = lv_label_create(tcp_connect_btn_);
    lv_label_set_text(tcp_btn_label, "Connect");
    lv_obj_set_style_text_font(tcp_btn_label, &lv_font_montserrat_20, 0);
    lv_obj_center(tcp_btn_label);
    lv_obj_add_event_fn(tcp_connect_btn_, LV_EVENT_CLICKED, [this](lv_event_t *) {
        start_connect_tcp(lv_textarea_get_text(tcp_addr_));
    });

    // Recent targets. Filled from NVS (tab5adb/tcp_history); a tap selects an
    // entry. Shows a "No recent connections" placeholder while empty.
    tcp_history_box_ = lv_obj_create(tcp_card);
    lv_obj_remove_style_all(tcp_history_box_);
    lv_obj_set_width(tcp_history_box_, LV_PCT(100));
    lv_obj_set_height(tcp_history_box_, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tcp_history_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tcp_history_box_, 2, 0);
    rebuild_tcp_history();

    // ---- Bottom navigation cards ----
    lv_obj_t *nav = lv_obj_create(body);
    lv_obj_remove_style_all(nav);
    lv_obj_set_size(nav, LV_PCT(100), 170);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(nav, 16, 0);
    lv_obj_remove_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_bottom(nav, 2, 0);

    auto nav_card = [&nav](const char *icon, const char *title,
                           std::function<void(lv_event_t *)> callback) {
        lv_obj_t *card = lv_obj_create(nav);
        lv_obj_set_height(card, LV_PCT(100));
        lv_obj_set_flex_grow(card, 1);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x444444), 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(card, 2, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(card, 14, 0);
        lv_obj_add_event_fn(card, LV_EVENT_CLICKED, std::move(callback));

        lv_obj_t *icon_label = lv_label_create(card);
        lv_label_set_text(icon_label, icon);
        lv_obj_set_style_text_font(icon_label, R.font.lucide_40, 0);
        lv_obj_set_style_text_color(icon_label, lv_color_hex(0x455a64), 0);

        lv_obj_t *title_label = lv_label_create(card);
        lv_label_set_text(title_label, title);
        lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    };
    nav_card(LUCIDE_INFO, "About", [](lv_event_t *) {
        screen_manager.push(std::make_shared<AboutScreen>());
    });
    nav_card(LUCIDE_HARD_DRIVE, "Files", [](lv_event_t *) {
        // The Tab5 SD card is browsable without an adb connection.
        screen_manager.push(std::make_shared<SDFileBrowserScreen>());
    });
    nav_card(LUCIDE_SETTINGS, "Settings", [](lv_event_t *) {
        screen_manager.push(std::make_shared<SettingsScreen>());
    });
}

void HomeScreen::onAppear() {
    // Register (cheap, no bring-up) so the status line tracks live transitions
    // while Home is visible, then show the current state.
    std::weak_ptr<wifi::Listener> wl(std::shared_ptr<wifi::Listener>(
        shared_from_this(), static_cast<wifi::Listener *>(this)));
    wifi::manager().set_listener(wl);
    refresh_wifi_status();
    rebuild_tcp_history();  // a connect since we last appeared may have added one
}

void HomeScreen::on_wifi_state(const wifi::Status & /*status*/) {
    lv_async_call([self = shared_from_this(), this]() {
        if (exited()) return;
        refresh_wifi_status();
        update_tcp_controls_enabled();
    });
}

void HomeScreen::refresh_wifi_status() {
    if (!wifi_status_) return;
    auto st = wifi::manager().status();
    if (st.state == wifi::State::Connected) {
        lv_label_set_text_fmt(wifi_status_, "Connected to %s", st.ssid.c_str());
        lv_obj_set_style_text_color(wifi_status_, lv_color_hex(0x2e7d32), 0);
        return;
    }
    if (st.state == wifi::State::Connecting) {
        lv_label_set_text_fmt(wifi_status_, "Connecting to %s...", st.ssid.c_str());
        lv_obj_set_style_text_color(wifi_status_, lv_color_hex(0x90a4ae), 0);
        return;
    }
    if (st.state == wifi::State::Off) {
        lv_label_set_text(wifi_status_, "Wi-Fi off");
        lv_obj_set_style_text_color(wifi_status_, lv_color_hex(0x90a4ae), 0);
        return;
    }
    std::string saved = wifi::manager().saved_ssid();
    if (!saved.empty())
        lv_label_set_text_fmt(wifi_status_, "Wi-Fi: saved network %s", saved.c_str());
    else
        lv_label_set_text(wifi_status_, "Wi-Fi: not configured");
    lv_obj_set_style_text_color(wifi_status_, lv_color_hex(0x90a4ae), 0);
}

void HomeScreen::rebuild_tcp_history() {
    if (!tcp_history_box_) return;
    lv_obj_clean(tcp_history_box_);  // drop the old rows

    std::vector<std::string> targets = app::tcp_history::load();
    if (targets.empty()) {
        // No connections yet: keep the list area, show a placeholder in its place.
        lv_obj_t *empty = lv_label_create(tcp_history_box_);
        lv_label_set_text(empty, "No recent connections");
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(0xb0bec5), 0);
        lv_obj_set_style_margin_top(empty, 12, 0);
        update_tcp_controls_enabled();
        return;
    }

    lv_obj_t *heading = lv_label_create(tcp_history_box_);
    lv_label_set_text(heading, "Recent");
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(0x90a4ae), 0);
    lv_obj_set_style_margin_top(heading, 12, 0);

    bool first = true;
    for (const std::string &target : targets) {
        if (!first) {  // thin divider between rows (WiFiScreen / Settings style)
            lv_obj_t *sep = lv_obj_create(tcp_history_box_);
            lv_obj_remove_style_all(sep);
            lv_obj_set_size(sep, LV_PCT(100), 1);
            lv_obj_set_style_bg_color(sep, lv_color_hex(0xdddddd), 0);
            lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        }
        first = false;

        lv_obj_t *row = lv_obj_create(tcp_history_box_);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_ver(row, 10, 0);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, LUCIDE_HISTORY);
        lv_obj_set_style_text_font(icon, R.font.lucide_40, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(0x607d8b), 0);

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, target.c_str());
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x37474f), 0);

        lv_obj_add_event_fn(row, LV_EVENT_CLICKED,
                            [this, target](lv_event_t *) { select_tcp_target(target); });
    }
    update_tcp_controls_enabled();
}

void HomeScreen::update_tcp_controls_enabled() {
    const bool connected = wifi::manager().status().state == wifi::State::Connected;
    auto set_enabled = [connected](lv_obj_t *o) {
        if (!o) return;
        if (connected)
            lv_obj_remove_state(o, LV_STATE_DISABLED);
        else
            lv_obj_add_state(o, LV_STATE_DISABLED);
    };
    set_enabled(tcp_addr_);
    set_enabled(tcp_connect_btn_);

    // Recent-target rows: disable (block taps) + dim the whole list when offline.
    // The "No recent connections" placeholder isn't clickable, so it's left alone.
    if (tcp_history_box_) {
        lv_obj_set_style_opa(tcp_history_box_, connected ? LV_OPA_COVER : LV_OPA_50, 0);
        uint32_t n = lv_obj_get_child_count(tcp_history_box_);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *child = lv_obj_get_child(tcp_history_box_, i);
            if (lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) set_enabled(child);
        }
    }
}

void HomeScreen::select_tcp_target(const std::string &target) {
    if (tcp_addr_) lv_textarea_set_text(tcp_addr_, target.c_str());
    start_connect_tcp(target);
}

void HomeScreen::show_tcp_keyboard() {
    if (tcp_keyboard_) return;  // already up
    // Pinned full-width to the screen bottom (a child of root_, taken out of the
    // flex layout), WiFiScreen-style. Its OK key triggers the connect.
    constexpr int kKeyboardH = 420;
    tcp_keyboard_ = lv_keyboard_create(root_);
    lv_obj_add_flag(tcp_keyboard_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(tcp_keyboard_, PANEL_W, kKeyboardH);
    lv_obj_align(tcp_keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(tcp_keyboard_, tcp_addr_);
    // OK confirms = connect to the typed address. Deferred via lv_async_call so the
    // keyboard is torn down OUTSIDE its own in-flight event (tearing an lv_keyboard
    // down mid-event hangs LVGL).
    lv_obj_add_event_fn(tcp_keyboard_, LV_EVENT_READY, [this](lv_event_t *) {
        std::string addr = lv_textarea_get_text(tcp_addr_);
        lv_async_call([self = shared_from_this(), this, addr]() {
            if (exited()) return;
            start_connect_tcp(addr);
        });
    });
    // Tapping the X/close key (CANCEL) just hides the keyboard.
    lv_obj_add_event_fn(tcp_keyboard_, LV_EVENT_CANCEL, [this](lv_event_t *) {
        lv_async_call([self = shared_from_this(), this]() {
            if (exited()) return;
            hide_tcp_keyboard();
        });
    });
}

void HomeScreen::hide_tcp_keyboard() {
    if (tcp_keyboard_) {
        lv_obj_delete(tcp_keyboard_);
        tcp_keyboard_ = nullptr;
    }
}

// Split "host" or "host:port" into host + port (default 5555 when omitted). The
// last ':' is the separator (an IPv4/hostname has none in the host part). Returns
// false on an empty host or an out-of-range / non-numeric port.
static bool parse_host_port(const std::string &addr, std::string &host, uint16_t &port) {
    std::string s = addr;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (s.empty()) return false;

    size_t colon = s.rfind(':');
    if (colon == std::string::npos) {
        host = s;
        port = 5555;
        return true;
    }
    host = s.substr(0, colon);
    std::string p = s.substr(colon + 1);
    if (host.empty() || p.empty()) return false;
    long v = 0;
    for (char c : p) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
        if (v > 65535) return false;
    }
    if (v == 0) return false;
    port = static_cast<uint16_t>(v);
    return true;
}

void HomeScreen::start_connect() {
    // A modal scrim covers the cards for the whole connect flow, so the user
    // can't navigate away (or kick off TCP/IP) mid-connect.
    open_progress("Connecting...\nAllow USB debugging on the phone");
    app::adb_connect_async([this](bool ok) {
        proceed_after_connect(ok,
                              "Could not reach the device. Check the USB cable and "
                              "that USB debugging is allowed, then tap Connect again.");
    });
}

void HomeScreen::start_connect_tcp(const std::string &addr) {
    std::string host;
    uint16_t port = 0;
    if (!parse_host_port(addr, host, port)) {
        app::modal_message(root_, "Invalid address",
                           "Enter the device address as host or host:port "
                           "(e.g. 192.168.1.100:5555).");
        return;
    }
    hide_tcp_keyboard();
    std::string target = host + ":" + std::to_string(port);
    open_progress((std::string("Connecting to ") + target + "...").c_str());
    app::adb_connect_tcp_async(host, port, [this, target](bool ok) {
        if (ok) app::tcp_history::add(target);  // remember a target that worked
        proceed_after_connect(ok,
                              "Could not reach the device. Check the address and that "
                              "wireless debugging is on, then try again.");
    });
}

void HomeScreen::proceed_after_connect(bool ok, const char *fail_msg) {
    // Two stages, both completing on the LVGL thread: the adb link (done — `ok`),
    // then the eager tab5adb-agent bring-up that decides Normal vs Limited mode
    // (AgentClient records the mode). The device screen is pushed once the mode is
    // known — success or failure both proceed; Limited just hides the agent-backed
    // features — so every screen can read a settled mode at build time.
    if (!ok) {
        close_progress();
        app::modal_message(root_, "Connection failed", fail_msg);
        return;
    }
    // In Limited mode ensure_connected short-circuits (no agent is started), so
    // don't show the misleading "starting agent" status for that path.
    if (app::android_mode() != app::AndroidMode::Limited)
        set_progress("Starting agent on the phone...");
    app::agent_client().ensure_connected([this](bool /*agent_ok*/) {
        close_progress();
        screen_manager.push(std::make_shared<ADBDeviceScreen>());
    });
}

void HomeScreen::open_progress(const char *message) {
    if (progress_card_) return;
    progress_card_ = app::modal_open(root_);

    auto header = lv_obj_create(progress_card_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 24, 0);
    auto spinner = lv_spinner_create(header);
    lv_obj_set_size(spinner, 64, 64);
    lv_obj_set_style_align(spinner, LV_ALIGN_CENTER, 0);
    auto title_label = lv_label_create(header);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_28, 0);
    lv_label_set_text(title_label, "Connecting");

    progress_label_ = lv_label_create(progress_card_);
    lv_obj_set_width(progress_label_, LV_PCT(100));
    lv_label_set_long_mode(progress_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(progress_label_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(progress_label_, lv_color_hex(0x444444), 0);
    lv_label_set_text(progress_label_, message);
}

void HomeScreen::set_progress(const char *message) {
    if (progress_label_) lv_label_set_text(progress_label_, message);
}

void HomeScreen::close_progress() {
    if (progress_card_) app::modal_close(progress_card_);
    progress_card_ = nullptr;
    progress_label_ = nullptr;
}
