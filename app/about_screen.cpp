#include "about_screen.hpp"

#include <functional>

#include "adb_app.hpp"
#include "app_version.hpp"
#include "modal.hpp"
#include "resources/resources.h"
#include "screen_manager.hpp"
#include "third_party_licenses.hpp"

// ---- Editable about-page content ------------------------------------------
namespace {
constexpr const char kAppName[] = "Tab5-ADB";
constexpr const char kTagline[] = "ADB client for M5Stack Tab5";
constexpr const char kAuthorName[] = "Hiroki Kawakami";

constexpr const char kRepoUrl[] = "https://github.com/Hiroki-Kawakami/Tab5-ADB";
constexpr const char kProfileUrl[] = "https://github.com/Hiroki-Kawakami";
constexpr const char kXUrl[] = "https://x.com/hiroki_cockatoo";
}  // namespace

namespace {

// A bordered block matching the SettingsScreen card look (theme card border +
// radius, no inner scroll, vertical flex).
lv_obj_t *make_block(lv_obj_t *parent) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(obj, 20, 0);
    lv_obj_set_style_pad_row(obj, 16, 0);
    return obj;
}

lv_obj_t *make_separator(lv_obj_t *parent) {
    lv_obj_t *sep = lv_obj_create(parent);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0xdddddd), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    return sep;
}

// A read-only row: leading icon, then a title (and optional value below it).
lv_obj_t *make_info_row(lv_obj_t *parent, const char *icon, const char *title,
                        const char *value) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 16, 0);

    lv_obj_t *icon_label = lv_label_create(r);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, R.font.lucide_40, 0);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(0x607d8b), 0);

    lv_obj_t *col = lv_obj_create(r);
    lv_obj_remove_style_all(col);
    // Plain lv_objs are clickable by default and don't bubble — drop it so the
    // tap reaches the row's own CLICKED handler (the device-summary-header gotcha).
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_set_flex_grow(col, 1);

    lv_obj_t *title_label = lv_label_create(col);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_28, 0);

    if (value && value[0]) {
        lv_obj_t *value_label = lv_label_create(col);
        lv_label_set_text(value_label, value);
        lv_obj_set_style_text_font(value_label, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(value_label, lv_color_hex(0x90a4ae), 0);
    }
    return r;
}

}  // namespace

void AboutScreen::build() {
    lv_obj_set_size(root_, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0xeeeeee), 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Navigation bar (back + title) — FileManager/Settings-style 120px ----
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
    lv_label_set_text(title_label, "About");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_38, 0);

    // ---- Body (scrolls as a whole) ----
    lv_obj_t *body = lv_obj_create(root_);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 24, 0);
    lv_obj_set_style_pad_row(body, 24, 0);

    // ---- App identity block ----
    {
        lv_obj_t *app_block = make_block(body);
        lv_obj_set_flex_align(app_block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_top(app_block, 32, 0);

        lv_obj_t *mark = lv_label_create(app_block);
        lv_label_set_text(mark, LUCIDE_TABLET_SMARTPHONE);
        lv_obj_set_style_text_font(mark, R.font.lucide_40, 0);
        lv_obj_set_style_text_color(mark, lv_color_hex(0x455a64), 0);

        lv_obj_t *name = lv_label_create(app_block);
        lv_label_set_text(name, kAppName);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(0x263238), 0);

        lv_obj_t *tagline = lv_label_create(app_block);
        lv_label_set_text(tagline, kTagline);
        lv_obj_set_style_text_font(tagline, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(tagline, lv_color_hex(0x90a4ae), 0);

        lv_obj_t *version = lv_label_create(app_block);
        lv_label_set_text(version, kAppVersion);
        lv_obj_set_style_text_font(version, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(version, lv_color_hex(0x607d8b), 0);

        make_separator(app_block);
        make_info_row(app_block, LUCIDE_USER, kAuthorName, "Author");
    }

    // ---- Links block (each row taps to a QR modal so it opens on the phone) ----
    {
        lv_obj_t *links_block = make_block(body);
        lv_obj_set_style_pad_ver(links_block, 16, 0);
        lv_obj_set_style_pad_row(links_block, 16, 0);

        struct Link {
            const char *icon;
            const char *title;
            const char *url;
        };
        const Link links[] = {
            {LUCIDE_CODE, "Repository", kRepoUrl},
            {LUCIDE_USER, "GitHub Profile", kProfileUrl},
            {LUCIDE_AT_SIGN, "X", kXUrl},
        };
        bool first = true;
        for (const Link &l : links) {
            if (!first) make_separator(links_block);
            first = false;

            lv_obj_t *row = make_info_row(links_block, l.icon, l.title, l.url);
            lv_obj_set_style_pad_ver(row, 12, 0);
            lv_obj_set_style_radius(row, 8, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

            // Trailing QR glyph hinting the row is scannable.
            lv_obj_t *hint = lv_label_create(row);
            lv_label_set_text(hint, LUCIDE_QR_CODE);
            lv_obj_set_style_text_font(hint, R.font.lucide_40, 0);
            lv_obj_set_style_text_color(hint, lv_color_hex(0xb0bec5), 0);

            const char *title = l.title;
            const char *url = l.url;
            lv_obj_add_event_fn(row, LV_EVENT_CLICKED, [this, title, url](lv_event_t *) {
                show_link_qr(title, url);
            });
        }
    }

    // ---- Acknowledgements button (pushes the licenses screen — lands later) ----
    {
        lv_obj_t *ack = lv_button_create(body);
        lv_obj_remove_style_all(ack);
        lv_obj_set_size(ack, LV_PCT(100), 80);
        lv_obj_set_style_bg_color(ack, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(ack, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ack, 1, 0);
        lv_obj_set_style_border_color(ack, lv_color_hex(0x444444), 0);
        lv_obj_set_style_radius(ack, 12, 0);
        lv_obj_set_style_bg_color(ack, lv_color_hex(0xe0e0e0), LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(ack, 2, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(ack, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ack, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(ack, 32, 0);
        lv_obj_set_style_pad_column(ack, 24, 0);

        lv_obj_t *ack_icon = lv_label_create(ack);
        lv_label_set_text(ack_icon, LUCIDE_SCALE);
        lv_obj_set_style_text_font(ack_icon, R.font.lucide_40, 0);
        lv_obj_t *ack_text = lv_label_create(ack);
        lv_label_set_text(ack_text, "Acknowledgements");
        lv_obj_set_style_text_font(ack_text, &lv_font_montserrat_28, 0);
        lv_obj_set_flex_grow(ack_text, 1);
        lv_obj_t *ack_chevron = lv_label_create(ack);
        lv_label_set_text(ack_chevron, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(ack_chevron, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(ack_chevron, lv_color_hex(0xc0c0c0), 0);

        lv_obj_add_event_fn(ack, LV_EVENT_CLICKED, [](lv_event_t *) {
            screen_manager.push(std::make_shared<AcknowledgementsScreen>());
        });
    }
}

void AboutScreen::show_link_qr(const char *title, const char *url) {
    lv_obj_t *card = app::modal_open(root_);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 20, 0);

    lv_obj_t *title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_28, 0);

    lv_obj_t *qr = lv_qrcode_create(card);
    lv_qrcode_set_size(qr, 320);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, url, lv_strlen(url));
    // A white quiet-zone border so scanners lock on against the card.
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 12, 0);

    lv_obj_t *url_label = lv_label_create(card);
    lv_label_set_text(url_label, url);
    lv_label_set_long_mode(url_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(url_label, LV_PCT(100));
    lv_obj_set_style_text_align(url_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(url_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(url_label, lv_color_hex(0x607d8b), 0);

    lv_obj_t *close = lv_button_create(card);
    lv_obj_set_size(close, LV_PCT(100), 64);
    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, "Close");
    lv_obj_set_style_text_font(close_label, &lv_font_montserrat_20, 0);
    lv_obj_center(close_label);
    lv_obj_add_event_fn(close, LV_EVENT_CLICKED,
                        [card](lv_event_t *) { app::modal_close(card); });
}

void AcknowledgementsScreen::build() {
    lv_obj_set_size(root_, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0xeeeeee), 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Navigation bar (back + title) — matches the AboutScreen 120px bar ----
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
    lv_label_set_text(title_label, "Acknowledgements");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_38, 0);

    // ---- Scrollable license text ----
    lv_obj_t *body = lv_obj_create(root_);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_pad_all(body, 24, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    lv_obj_t *text = lv_label_create(body);
    lv_label_set_text_static(text, app::kThirdPartyLicenses);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(text, LV_PCT(100));
    lv_obj_set_style_text_font(text, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(text, lv_color_hex(0x37474f), 0);
}
