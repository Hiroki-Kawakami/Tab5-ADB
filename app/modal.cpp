#include "modal.hpp"

#include <utility>

#include "lvgl.hpp"

namespace app {

namespace {

lv_obj_t *modal_button(lv_obj_t *row, const char *text) {
    auto button = lv_button_create(row);
    lv_obj_set_height(button, 72);
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_style_radius(button, 12, 0);
    auto label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

// title/text/button-row scaffolding shared by confirm and message.
lv_obj_t *modal_card(lv_obj_t *parent, const char *title, const char *text) {
    auto card = modal_open(parent);
    auto title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_28, 0);
    auto text_label = lv_label_create(card);
    lv_label_set_text(text_label, text);
    lv_obj_set_width(text_label, LV_PCT(100));
    lv_label_set_long_mode(text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(text_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(text_label, lv_color_hex(0x444444), 0);

    auto row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 24, 0);
    return card;
}

lv_obj_t *button_row(lv_obj_t *card) {
    return lv_obj_get_child(card, -1);
}

}  // namespace

lv_obj_t *modal_open(lv_obj_t *parent) {
    auto scrim = lv_obj_create(parent);
    lv_obj_remove_style_all(scrim);
    // The parent is typically a flex-layout screen root: take the scrim out of
    // the layout and pin it over the whole screen.
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);  // absorb taps behind the card
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);

    auto card = lv_obj_create(scrim);
    lv_obj_set_width(card, 600);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 32, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 24, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

void modal_close(lv_obj_t *card) {
    lv_obj_delete(lv_obj_get_parent(card));  // the scrim owns the card
}

void modal_confirm(lv_obj_t *parent, const char *title, const char *text,
                   const char *ok_text, bool destructive,
                   std::function<void()> on_ok) {
    auto card = modal_card(parent, title, text);
    auto row = button_row(card);

    auto cancel = modal_button(row, "Cancel");
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_color(cancel, lv_color_black(), 0);
    lv_obj_add_event_fn(cancel, LV_EVENT_CLICKED, [card](lv_event_t*){
        modal_close(card);
    });

    auto ok = modal_button(row, ok_text);
    if (destructive) lv_obj_set_style_bg_color(ok, lv_color_hex(0xd32f2f), 0);
    lv_obj_add_event_fn(ok, LV_EVENT_CLICKED, [card, on_ok = std::move(on_ok)](lv_event_t*){
        // Closing the dialog frees this lambda: copy the callback out first.
        auto cb = on_ok;
        modal_close(card);
        if (cb) cb();
    });
}

void modal_message(lv_obj_t *parent, const char *title, const char *text) {
    auto card = modal_card(parent, title, text);
    auto ok = modal_button(button_row(card), "OK");
    lv_obj_add_event_fn(ok, LV_EVENT_CLICKED, [card](lv_event_t*){
        modal_close(card);
    });
}

}  // namespace app
