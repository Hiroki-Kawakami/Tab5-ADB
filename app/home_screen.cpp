#include "home_screen.hpp"

#include <memory>

#include "adb_app.hpp"
#include "adb_device_screen.hpp"
#include "screen_manager.hpp"

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
}

void HomeScreen::start_connect() {
    lv_obj_add_state(connect_btn_, LV_STATE_DISABLED);
    lv_label_set_text(status_label_, "Connecting... allow USB debugging on the phone");

    app::adb_connect_async([this](bool ok) {
        if (ok) {
            screen_manager.push(std::make_shared<ADBDeviceScreen>());
        } else {
            lv_obj_remove_state(connect_btn_, LV_STATE_DISABLED);
            lv_label_set_text(status_label_, "Connection failed. Tap Connect to retry.");
        }
    });
}
