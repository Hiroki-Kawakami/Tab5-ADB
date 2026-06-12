#include "settings_screen.hpp"

#include "adb_app.hpp"
#include "bsp.h"
#include "modal.hpp"
#include "resources/resources.h"
#include "screen_manager.hpp"
#include "settings.hpp"

void SettingsScreen::build() {
    lv_obj_set_size(root_, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0xeeeeee), 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Navigation bar (back + title) — FileManager-style 120px bar ----
    lv_obj_t *navigation = lv_obj_create(root_);
    lv_obj_remove_style_all(navigation);
    lv_obj_set_size(navigation, LV_PCT(100), 120);
    lv_obj_set_style_bg_color(navigation, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(navigation, LV_OPA_COVER, 0);
    lv_obj_remove_flag(navigation, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(navigation, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(navigation, 1, 0);
    lv_obj_set_style_border_color(navigation, lv_color_hex(0xc0c0c0), 0);
    lv_obj_set_flex_flow(navigation, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(navigation, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(navigation, 24, 0);
    lv_obj_set_style_pad_column(navigation, 24, 0);

    lv_obj_t *back = lv_button_create(navigation);
    lv_obj_remove_style_all(back);
    lv_obj_set_flex_flow(back, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(back, 8, 0);
    lv_obj_set_style_pad_column(back, 16, 0);
    lv_obj_add_event_fn(back, LV_EVENT_CLICKED,
                        [](lv_event_t *) { screen_manager.pop(); });
    lv_obj_set_style_bg_color(back, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_38, 0);
    lv_obj_set_style_pad_all(back_label, 8, 0);
    lv_obj_t *title_label = lv_label_create(back);
    lv_label_set_text(title_label, "Settings");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_38, 0);

    // ---- Body ----
    body_ = lv_obj_create(root_);
    lv_obj_remove_style_all(body_);
    lv_obj_set_width(body_, LV_PCT(100));
    lv_obj_set_flex_grow(body_, 1);
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body_, 24, 0);
    lv_obj_set_style_pad_row(body_, 24, 0);

    // ---- Wi-Fi settings button ----
    auto wifi_button = lv_button_create(body_);
    lv_obj_remove_style_all(wifi_button);
    lv_obj_set_size(wifi_button, LV_PCT(100), 80);
    lv_obj_set_style_bg_color(wifi_button, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(wifi_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_button, 1, 0);
    lv_obj_set_style_border_color(wifi_button, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(wifi_button, 12, 0);
    lv_obj_set_style_bg_color(wifi_button, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(wifi_button, 2, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(wifi_button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi_button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(wifi_button, 32, 0);
    lv_obj_set_style_pad_column(wifi_button, 24, 0);
    lv_obj_add_event_fn(wifi_button, LV_EVENT_CLICKED, [](lv_event_t*) {
        // screen_manager.push(std::make_shared<WiFiSettingsScreen>());
    });
    auto wifi_icon = lv_label_create(wifi_button);
    lv_label_set_text(wifi_icon, LUCIDE_WIFI);
    lv_obj_set_style_text_font(wifi_icon, R.font.lucide_40, 0);
    auto wifi_text = lv_label_create(wifi_button);
    lv_label_set_text(wifi_text, "Wi-Fi");
    lv_obj_set_style_text_font(wifi_text, &lv_font_montserrat_28, 0);
    lv_obj_set_flex_grow(wifi_text, 1);
    auto wifi_chevron = lv_label_create(wifi_button);
    lv_label_set_text(wifi_chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(wifi_chevron, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(wifi_chevron, lv_color_hex(0xc0c0c0), 0);

    auto block = [this](const char *title) {
        auto title_label = lv_label_create(body_);
        lv_label_set_text(title_label, title);
        lv_obj_set_style_pad_top(title_label, 24, 0);

        auto obj = lv_obj_create(body_);
        lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
        return obj;
    };
    auto separator = [this](){
        auto sep = lv_obj_create(body_);
        lv_obj_remove_style_all(sep);
        lv_obj_set_style_bg_color(sep, lv_color_hex(0xc0c0c0), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        lv_obj_set_size(sep, LV_PCT(100), 1);
        lv_obj_set_style_margin_hor(sep, 24, 0);
    };

    // A settings row: a title, then any trailing control the caller appends sits
    // on the right (the title grows to push it there). No icon — Lucide has no
    // consistent glyph lineup covering every setting.
    auto setting_row = [](lv_obj_t *parent, const char *text) {
        auto r = lv_obj_create(parent);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 20, 0);
        auto tx = lv_label_create(r);
        lv_label_set_text(tx, text);
        lv_obj_set_style_text_font(tx, &lv_font_montserrat_28, 0);
        lv_obj_set_flex_grow(tx, 1);  // pushes any trailing control to the right
        return r;
    };

    // ---- Display Settings block ----
    {
        auto display_block = block("Display");
        lv_obj_set_style_pad_all(display_block, 20, 0);
        lv_obj_set_style_pad_row(display_block, 16, 0);

        // -- Brightness (header row + full-width slider) --
        auto bright = lv_obj_create(display_block);
        lv_obj_remove_style_all(bright);
        lv_obj_set_size(bright, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_remove_flag(bright, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(bright, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(bright, 12, 0);

        auto bright_head = setting_row(bright, "Brightness");
        auto value_label = lv_label_create(bright_head);
        lv_label_set_text_fmt(value_label, "%d%%", app::display_brightness());
        lv_obj_set_style_text_font(value_label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(value_label, lv_color_hex(0x808080), 0);

        auto slider = lv_slider_create(bright);
        lv_obj_set_width(slider, LV_PCT(100));
        // Floor at 1: brightness 0 fully blanks the backlight (nothing visible).
        lv_slider_set_range(slider, 1, 100);
        lv_slider_set_value(slider, app::display_brightness(), LV_ANIM_OFF);
        // The knob is centered on the track end at 0%/100%, so it overflows the
        // track by half its size at each extreme. Give it an explicit size and a
        // half-knob margin all round so neither end (nor top/bottom) is clipped.
        constexpr int kKnob = 28;
        lv_obj_set_style_width(slider, kKnob, LV_PART_KNOB);
        lv_obj_set_style_height(slider, kKnob, LV_PART_KNOB);
        lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_margin_all(slider, kKnob / 2, 0);
        // Apply live while dragging; persist on release (avoid an NVS write per tick).
        lv_obj_add_event_fn(slider, LV_EVENT_VALUE_CHANGED,
                            [slider, value_label](lv_event_t *) {
            int v = lv_slider_get_value(slider);
            bsp_display_set_brightness(v);
            lv_label_set_text_fmt(value_label, "%d%%", v);
        });
        lv_obj_add_event_fn(slider, LV_EVENT_RELEASED, [slider](lv_event_t *) {
            app::set_display_brightness(lv_slider_get_value(slider));
        });

        // thin divider
        auto sep = lv_obj_create(display_block);
        lv_obj_remove_style_all(sep);
        lv_obj_set_size(sep, LV_PCT(100), 1);
        lv_obj_set_style_bg_color(sep, lv_color_hex(0xdddddd), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

        // -- Color depth (16-bit / 24-bit segmented toggle) --
        auto color_head = setting_row(display_block, "Color Mode");

        auto seg = lv_obj_create(color_head);
        lv_obj_remove_style_all(seg);
        lv_obj_set_size(seg, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_all(seg, 4, 0);
        lv_obj_set_style_pad_column(seg, 4, 0);
        lv_obj_set_style_radius(seg, 12, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(0xe0e0e0), 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);

        auto seg_btn = [](lv_obj_t *parent, const char *txt) {
            auto b = lv_button_create(parent);
            lv_obj_remove_style_all(b);
            lv_obj_set_style_radius(b, 10, 0);
            lv_obj_set_style_pad_hor(b, 22, 0);
            lv_obj_set_style_pad_ver(b, 12, 0);
            auto l = lv_label_create(b);
            lv_label_set_text(l, txt);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
            lv_obj_center(l);
            return b;
        };
        auto btn16 = seg_btn(seg, "16-bit");
        auto btn24 = seg_btn(seg, "24-bit");

        // Paint the active half blue; read the persisted value so it survives
        // re-entry (the live display only changes after a restart).
        auto refresh_seg = [btn16, btn24]() {
            bool is16 = app::display_color_depth() == app::ColorDepth::Color16;
            struct { lv_obj_t *b; bool on; } items[] = {{btn16, is16}, {btn24, !is16}};
            for (auto &it : items) {
                lv_obj_set_style_bg_color(
                    it.b, it.on ? lv_color_hex(0x2196f3) : lv_color_hex(0xe0e0e0), 0);
                lv_obj_set_style_bg_opa(it.b, LV_OPA_COVER, 0);
                lv_obj_set_style_text_color(
                    lv_obj_get_child(it.b, 0),
                    it.on ? lv_color_white() : lv_color_hex(0x404040), 0);
            }
        };
        refresh_seg();

        auto choose = [this, refresh_seg](app::ColorDepth d) {
            if (app::display_color_depth() == d) return;  // no change
            app::set_display_color_depth(d);
            refresh_seg();
            // Boot-fixed: the framebuffer format is allocated at bsp_init, so the
            // change only shows after a restart.
            app::modal_confirm(root_, "Restart required",
                               "The color depth changes after a restart.\n"
                               "Restart now?",
                               "Restart", false, []() { bsp_restart(); });
        };
        lv_obj_add_event_fn(btn16, LV_EVENT_CLICKED,
                            [choose](lv_event_t *) { choose(app::ColorDepth::Color16); });
        lv_obj_add_event_fn(btn24, LV_EVENT_CLICKED,
                            [choose](lv_event_t *) { choose(app::ColorDepth::Color24); });
    }
}
