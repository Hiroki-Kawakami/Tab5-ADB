#include "home_screen.hpp"

#include <memory>

#include "adb_app.hpp"
#include "adb_device_screen.hpp"
#include "agent_client.hpp"
#include "screen_manager.hpp"
#include "sd_file_browser_screen.hpp"

void HomeScreen::build() {
    lv_obj_set_size(root_, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(root_);
    lv_label_set_text(title, "Tab5 ADB");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -100);

    connect_btn_ = lv_button_create(root_);
    lv_obj_set_size(connect_btn_, 260, 90);
    lv_obj_align(connect_btn_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *btn_label = lv_label_create(connect_btn_);
    lv_label_set_text(btn_label, "Connect");
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_20, 0);
    lv_obj_center(btn_label);

    status_label_ = lv_label_create(root_);
    lv_label_set_text(status_label_, "");
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0xb0bec5), 0);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 90);

    lv_obj_add_event_fn(connect_btn_, LV_EVENT_CLICKED,
                        [this](lv_event_t *) { start_connect(); });

    // The Tab5 SD card is browsable without an adb connection.
    lv_obj_t *sd_btn = lv_button_create(root_);
    lv_obj_set_size(sd_btn, 260, 90);
    lv_obj_align(sd_btn, LV_ALIGN_CENTER, 0, 200);
    lv_obj_set_style_bg_color(sd_btn, lv_color_hex(0x37474f), 0);
    lv_obj_t *sd_label = lv_label_create(sd_btn);
    lv_label_set_text(sd_label, "SD Card");
    lv_obj_set_style_text_font(sd_label, &lv_font_montserrat_20, 0);
    lv_obj_center(sd_label);
    lv_obj_add_event_fn(sd_btn, LV_EVENT_CLICKED, [](lv_event_t *) {
        screen_manager.push(std::make_shared<SDFileBrowserScreen>());
    });
}

void HomeScreen::start_connect() {
    lv_obj_add_state(connect_btn_, LV_STATE_DISABLED);
    lv_label_set_text(status_label_, "Connecting... allow USB debugging on the phone");

    // Two stages, both completing on the LVGL thread: the adb link, then the eager
    // tab5adb-agent bring-up that decides Normal vs Limited mode (AgentClient
    // records the mode). The device screen is pushed once the mode is known —
    // success or failure both proceed; Limited just hides the agent-backed
    // features — so every screen can read a settled mode at build time.
    app::adb_connect_async([this](bool ok) {
        if (!ok) {
            lv_obj_remove_state(connect_btn_, LV_STATE_DISABLED);
            lv_label_set_text(status_label_, "Connection failed. Tap Connect to retry.");
            return;
        }
        lv_label_set_text(status_label_, "Starting agent on the phone...");
        app::agent_client().ensure_connected([this](bool /*agent_ok*/) {
            screen_manager.push(std::make_shared<ADBDeviceScreen>());
        });
    });
}
