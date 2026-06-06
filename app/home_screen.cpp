#include "home_screen.hpp"
#include "adb_app.hpp"

void HomeScreen::build() {
    lv_obj_set_size(root_, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x101418), 0);

    lv_obj_t *label = lv_label_create(root_);
    lv_label_set_text(label, "Tab5 ADB");
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_center(label);
}
