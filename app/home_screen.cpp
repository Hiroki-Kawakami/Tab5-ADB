#include "home_screen.hpp"

#include <functional>
#include <memory>

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
    card_title(make_row(tcp_card), LUCIDE_WIFI, "Wireless (TCP/IP)");

    wifi_status_ = lv_label_create(tcp_card);
    lv_obj_set_style_text_font(wifi_status_, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(wifi_status_, lv_color_hex(0x90a4ae), 0);
    refresh_wifi_status();  // Wi-Fi is set up from Settings; this card just shows status

    lv_obj_t *input_row = make_row(tcp_card);
    lv_obj_t *addr = lv_textarea_create(input_row);
    lv_textarea_set_one_line(addr, true);
    lv_textarea_set_placeholder_text(addr, "192.168.1.100:5555");
    lv_obj_set_style_text_font(addr, &lv_font_montserrat_20, 0);
    lv_obj_set_height(addr, 60);
    lv_obj_set_flex_grow(addr, 1);
    lv_obj_add_state(addr, LV_STATE_DISABLED);
    lv_obj_t *tcp_btn = lv_button_create(input_row);
    lv_obj_set_size(tcp_btn, 160, 60);
    lv_obj_t *tcp_btn_label = lv_label_create(tcp_btn);
    lv_label_set_text(tcp_btn_label, "Connect");
    lv_obj_set_style_text_font(tcp_btn_label, &lv_font_montserrat_20, 0);
    lv_obj_center(tcp_btn_label);
    lv_obj_add_state(tcp_btn, LV_STATE_DISABLED);

    lv_obj_t *spacer = lv_obj_create(body);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_width(spacer, LV_PCT(100));
    lv_obj_set_flex_grow(spacer, 1);

    // ---- Bottom navigation cards ----
    lv_obj_t *nav = lv_obj_create(body);
    lv_obj_remove_style_all(nav);
    lv_obj_set_size(nav, LV_PCT(100), 170);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(nav, 16, 0);

    auto nav_card = [&nav](const char *icon, const char *title,
                           std::function<void(lv_event_t *)> callback) {
        lv_obj_t *card = lv_obj_create(nav);
        lv_obj_set_height(card, LV_PCT(100));
        lv_obj_set_flex_grow(card, 1);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(card, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
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
    nav_card(LUCIDE_INFO, "About", [](lv_event_t *) {});  // AboutScreen lands later
    nav_card(LUCIDE_HARD_DRIVE, "Files", [](lv_event_t *) {
        // The Tab5 SD card is browsable without an adb connection.
        screen_manager.push(std::make_shared<SDFileBrowserScreen>());
    });
    nav_card(LUCIDE_SETTINGS, "Settings", [](lv_event_t *) {
        screen_manager.push(std::make_shared<SettingsScreen>());
    });
}

void HomeScreen::onAppear() { refresh_wifi_status(); }

void HomeScreen::refresh_wifi_status() {
    if (!wifi_status_) return;
    auto st = wifi::manager().status();
    if (st.state == wifi::State::Connected) {
        lv_label_set_text_fmt(wifi_status_, "Connected to %s", st.ssid.c_str());
        lv_obj_set_style_text_color(wifi_status_, lv_color_hex(0x2e7d32), 0);
        return;
    }
    std::string saved = wifi::manager().saved_ssid();
    if (!saved.empty())
        lv_label_set_text_fmt(wifi_status_, "Wi-Fi: saved network %s", saved.c_str());
    else
        lv_label_set_text(wifi_status_, "Wi-Fi: not configured");
    lv_obj_set_style_text_color(wifi_status_, lv_color_hex(0x90a4ae), 0);
}

void HomeScreen::start_connect() {
    // A modal scrim covers the cards for the whole connect flow, so the user
    // can't navigate away (or kick off TCP/IP) mid-connect.
    open_progress("Connecting...\nAllow USB debugging on the phone");

    // Two stages, both completing on the LVGL thread: the adb link, then the eager
    // tab5adb-agent bring-up that decides Normal vs Limited mode (AgentClient
    // records the mode). The device screen is pushed once the mode is known —
    // success or failure both proceed; Limited just hides the agent-backed
    // features — so every screen can read a settled mode at build time.
    app::adb_connect_async([this](bool ok) {
        if (!ok) {
            close_progress();
            app::modal_message(root_, "Connection failed",
                               "Could not reach the device. Check the USB cable and "
                               "that USB debugging is allowed, then tap Connect again.");
            return;
        }
        // In Limited mode ensure_connected short-circuits (no agent is started),
        // so don't show the misleading "starting agent" status for that path.
        if (app::android_mode() != app::AndroidMode::Limited)
            set_progress("Starting agent on the phone...");
        app::agent_client().ensure_connected([this](bool /*agent_ok*/) {
            close_progress();
            screen_manager.push(std::make_shared<ADBDeviceScreen>());
        });
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
