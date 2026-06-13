#include "settings_screen.hpp"

#include <functional>
#include <initializer_list>
#include <memory>
#include <vector>

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

    // A segmented pill toggle: N options, the active one painted blue. Tapping a
    // different option repaints and fires on_select(index). Default is inline
    // (content width — sits on a setting_row's right). Pass fill=true to span the
    // parent width with equal-width options (for long labels on their own line).
    auto segmented = [](lv_obj_t *parent, std::initializer_list<const char *> labels,
                        int active, std::function<void(int)> on_select,
                        bool fill = false) {
        auto seg = lv_obj_create(parent);
        lv_obj_remove_style_all(seg);
        lv_obj_set_size(seg, fill ? LV_PCT(100) : LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_all(seg, 4, 0);
        lv_obj_set_style_pad_column(seg, 4, 0);
        lv_obj_set_style_radius(seg, 12, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(0xe0e0e0), 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);

        auto btns = std::make_shared<std::vector<lv_obj_t *>>();
        auto paint = [btns](int sel) {
            for (size_t i = 0; i < btns->size(); i++) {
                bool on = static_cast<int>(i) == sel;
                auto b = (*btns)[i];
                lv_obj_set_style_bg_color(
                    b, on ? lv_color_hex(0x2196f3) : lv_color_hex(0xe0e0e0), 0);
                lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
                lv_obj_set_style_text_color(
                    lv_obj_get_child(b, 0),
                    on ? lv_color_white() : lv_color_hex(0x404040), 0);
            }
        };
        int i = 0;
        for (auto txt : labels) {
            auto b = lv_button_create(seg);
            lv_obj_remove_style_all(b);
            lv_obj_set_style_radius(b, 10, 0);
            lv_obj_set_style_pad_hor(b, 22, 0);
            lv_obj_set_style_pad_ver(b, 12, 0);
            // flex_grow only when filling — in a content-sized seg it would
            // collapse the buttons to zero width.
            if (fill) lv_obj_set_flex_grow(b, 1);
            auto l = lv_label_create(b);
            lv_label_set_text(l, txt);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
            lv_obj_center(l);
            int idx = i++;
            lv_obj_add_event_fn(b, LV_EVENT_CLICKED, [paint, on_select, idx](lv_event_t *) {
                paint(idx);
                on_select(idx);
            });
            btns->push_back(b);
        }
        paint(active);
        return seg;
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
        segmented(color_head, {"16-bit", "24-bit"},
                  app::display_color_depth() == app::ColorDepth::Color16 ? 0 : 1,
                  [this](int idx) {
            auto d = idx == 0 ? app::ColorDepth::Color16 : app::ColorDepth::Color24;
            if (app::display_color_depth() == d) return;  // tapped the active one
            app::set_display_color_depth(d);
            // Boot-fixed: the framebuffer format is allocated at bsp_init, so the
            // change only shows after a restart.
            app::modal_confirm(root_, "Restart required",
                               "The color depth changes after a restart.\n"
                               "Restart now?",
                               "Restart", false, []() { bsp_restart(); });
        });
    }

    // ---- Audio Settings block ----
    {
        auto audio_block = block("Audio");
        lv_obj_set_style_pad_all(audio_block, 20, 0);
        lv_obj_set_style_pad_row(audio_block, 16, 0);

        // A thin in-block divider between rows.
        auto divider = [](lv_obj_t *parent) {
            auto d = lv_obj_create(parent);
            lv_obj_remove_style_all(d);
            lv_obj_set_size(d, LV_PCT(100), 1);
            lv_obj_set_style_bg_color(d, lv_color_hex(0xdddddd), 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        };

        // -- Master Volume (header row + slider 0..150, 100 = unity) --
        auto vol = lv_obj_create(audio_block);
        lv_obj_remove_style_all(vol);
        lv_obj_set_size(vol, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_remove_flag(vol, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(vol, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(vol, 12, 0);

        auto vol_head = setting_row(vol, "Master Volume");
        auto vol_value = lv_label_create(vol_head);
        lv_label_set_text_fmt(vol_value, "%d", app::master_volume());
        lv_obj_set_style_text_font(vol_value, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(vol_value, lv_color_hex(0x808080), 0);

        auto vslider = lv_slider_create(vol);
        lv_obj_set_width(vslider, LV_PCT(100));
        lv_slider_set_range(vslider, 0, 150);
        lv_slider_set_value(vslider, app::master_volume(), LV_ANIM_OFF);
        constexpr int kKnob = 28;
        lv_obj_set_style_width(vslider, kKnob, LV_PART_KNOB);
        lv_obj_set_style_height(vslider, kKnob, LV_PART_KNOB);
        lv_obj_set_style_radius(vslider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_margin_all(vslider, kKnob / 2, 0);
        // Apply live (sets the gain the next stream fades in to); persist on release.
        lv_obj_add_event_fn(vslider, LV_EVENT_VALUE_CHANGED,
                            [vslider, vol_value](lv_event_t *) {
            int v = lv_slider_get_value(vslider);
            bsp_audio_set_volume(v);  // 0..150; 100 = unity, >100 boosts (+6 dB max)
            lv_label_set_text_fmt(vol_value, "%d", v);
        });
        lv_obj_add_event_fn(vslider, LV_EVENT_RELEASED, [vslider](lv_event_t *) {
            app::set_master_volume(lv_slider_get_value(vslider));
        });

        divider(audio_block);

        // -- Speaker Output (Auto / Off; "always on" is intentionally not shown) --
        auto spk_head = setting_row(audio_block, "Speaker Output");
        segmented(spk_head, {"Off", "Auto"},
                  app::speaker_mode() == app::SpeakerMode::Off ? 0 : 1,
                  [](int idx) {
            auto m = idx == 0 ? app::SpeakerMode::Off : app::SpeakerMode::Auto;
            app::set_speaker_mode(m);
            bsp_audio_set_speaker_mode(m == app::SpeakerMode::Off
                                           ? BSP_AUDIO_SPEAKER_MODE_OFF
                                           : BSP_AUDIO_SPEAKER_MODE_AUTO);
        });

        divider(audio_block);

        // -- Equalizer (audio_dsp EQ stage on/off) --
        auto eq_head = setting_row(audio_block, "Equalizer");
        segmented(eq_head, {"Off", "On"}, app::equalizer_enabled() ? 1 : 0,
                  [](int idx) {
            bool on = idx == 1;
            app::set_equalizer_enabled(on);
            // The BSP override keeps this across HP insert/remove re-voicing.
            bsp_audio_set_eq_enabled(on);
        });

        divider(audio_block);

        // -- Sound Playback: which device plays the mirrored phone audio. The
        // option labels are long, so a dropdown keeps it on one row. --
        auto sp_head = setting_row(audio_block, "Sound Playback");
        auto dd = lv_dropdown_create(sp_head);
        lv_dropdown_set_options(dd, "M5Stack Tab5\nAndroid Device");
        lv_dropdown_set_selected(
            dd, app::audio_output_mode() == app::AudioOutputMode::PhoneOnly ? 1 : 0);
        lv_obj_set_style_text_font(dd, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_font(lv_dropdown_get_list(dd), &lv_font_montserrat_20, 0);
        lv_obj_set_width(dd, 280);
        lv_obj_add_event_fn(dd, LV_EVENT_VALUE_CHANGED, [dd](lv_event_t *) {
            app::set_audio_output_mode(lv_dropdown_get_selected(dd) == 1
                                           ? app::AudioOutputMode::PhoneOnly
                                           : app::AudioOutputMode::Tab5Only);
        });
    }

    // ---- Android Device block ----
    {
        auto android_block = block("Android Device");
        lv_obj_set_style_pad_all(android_block, 20, 0);
        lv_obj_set_style_pad_row(android_block, 16, 0);

        // -- Agent Mode (Normal uses tab5adb-agent / Limited is adb-only) --
        auto mode_head = setting_row(android_block, "Agent Mode");
        segmented(mode_head, {"Normal", "Limited"},
                  app::android_mode() == app::AndroidMode::Limited ? 1 : 0,
                  [](int idx) {
            app::set_android_mode(idx == 1 ? app::AndroidMode::Limited
                                           : app::AndroidMode::Normal);
        });

        // Explanatory caption (the option labels alone don't convey the trade-off).
        auto desc = lv_label_create(android_block);
        lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(desc, LV_PCT(100));
        lv_label_set_text(desc,
            "Normal runs tab5adb-agent on the phone for screen mirroring, the "
            "live preview, and app icons. Limited uses ADB only. Applies on the "
            "next connection.");
        lv_obj_set_style_text_font(desc, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(desc, lv_color_hex(0x808080), 0);
    }
}
