#include "adb_file_manager_screen.hpp"

#include <functional>

#include "adb_file_browser_screen.hpp"
#include "sd_file_browser_screen.hpp"
#include "screen_manager.hpp"
#include "resources/resources.h"

void ADBFileManagerScreen::build() {
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
    lv_label_set_text(title, "File Manager");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_38, 0);

    auto pad = lv_obj_create(navigation);
    lv_obj_remove_style_all(pad);
    lv_obj_set_flex_grow(pad, 1);

    auto refresh_button = lv_button_create(navigation);
    lv_obj_remove_style_all(refresh_button);
    lv_obj_set_style_pad_all(refresh_button, 16, 0);
    lv_obj_add_event_fn(refresh_button, LV_EVENT_CLICKED, [](lv_event_t*){});
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
    lv_obj_set_style_pad_all(list_, 24, 0);
    lv_obj_set_style_pad_row(list_, 24, 0);
    refresh();
}

void ADBFileManagerScreen::onExit() {
}

void ADBFileManagerScreen::refresh() {
    lv_obj_clean(list_);
    auto item = [this](std::string title, std::function<void()> on_click){
        auto button = lv_button_create(list_);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, LV_PCT(100), 160);
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
        lv_obj_add_event_fn(button, LV_EVENT_CLICKED, [on_click](lv_event_t*){
            on_click();
        });

        auto icon = lv_image_create(button);
        lv_image_set_src(icon, R.icon.hard_drive_40px);
        lv_obj_set_style_recolor(icon, lv_color_black(), 0);
        lv_obj_set_style_recolor_opa(icon, LV_OPA_COVER, 0);

        auto label = lv_label_create(button);
        lv_label_set_text(label, title.c_str());
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    };

    auto adb_browser = [](std::string path) {
        return [path]() {
            screen_manager.push(std::make_shared<ADBFileBrowserScreen>(path));
        };
    };
    auto adb_title = lv_label_create(list_);
    lv_label_set_text(adb_title, "ADB Device");
    item("Internal Storage", adb_browser("/sdcard"));
    item("System", adb_browser("/"));

    auto tab5_title = lv_label_create(list_);
    lv_label_set_text(tab5_title, "M5Stack Tab5");
    lv_obj_set_style_pad_top(tab5_title, 24, 0);
    item("SD Card", []() {
        screen_manager.push(std::make_shared<SDFileBrowserScreen>());
    });
}
