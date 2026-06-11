#include "term_keyboard.hpp"

#include <cstring>

namespace {

// Layer maps. lv_buttonmatrix_set_map keeps the pointer, so these are static.
// Layout (5 rows x 88px in a 720x440 area): digits on top, Tab/Ctrl/Shift as
// left anchors, Enter on the home row, shell-critical - / , . ' on base, the
// flat arrow row at the bottom. Shift swaps in uppercase + shifted punctuation
// (one-shot); Sym carries the remaining ASCII + Home..Del (latched).
const char *kMapBase[] = {
    "Esc", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
    LV_SYMBOL_BACKSPACE, "\n",
    "Tab", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "Ctrl", "a", "s", "d", "f", "g", "h", "j", "k", "l",
    LV_SYMBOL_NEW_LINE, "\n",
    "Shift", "z", "x", "c", "v", "b", "n", "m", ",", ".", "'", "\n",
    "?#&", "-", "/", " ", LV_SYMBOL_LEFT, LV_SYMBOL_UP, LV_SYMBOL_DOWN,
    LV_SYMBOL_RIGHT, "",
};

const char *kMapShift[] = {
    "Esc", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
    LV_SYMBOL_BACKSPACE, "\n",
    "Tab", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "Ctrl", "A", "S", "D", "F", "G", "H", "J", "K", "L",
    LV_SYMBOL_NEW_LINE, "\n",
    "Shift", "Z", "X", "C", "V", "B", "N", "M", "<", ">", "\"", "\n",
    "?#&", "_", "?", " ", LV_SYMBOL_LEFT, LV_SYMBOL_UP, LV_SYMBOL_DOWN,
    LV_SYMBOL_RIGHT, "",
};

const char *kMapSym[] = {
    "Esc", "!", "@", "#", "$", "%", "^", "&", "*", "(", ")",
    LV_SYMBOL_BACKSPACE, "\n",
    "Tab", "~", "`", "|", "\\", "{", "}", "[", "]", "<", ">", "\n",
    "Ctrl", ";", ":", "\"", "=", "+", "_", "?", LV_SYMBOL_NEW_LINE, "\n",
    "Shift", "Home", "End", "PgUp", "PgDn", "Ins", "Del", "\n",
    "?#&", "-", "/", " ", LV_SYMBOL_LEFT, LV_SYMBOL_UP, LV_SYMBOL_DOWN,
    LV_SYMBOL_RIGHT, "",
};

constexpr lv_buttonmatrix_ctrl_t W2 = LV_BUTTONMATRIX_CTRL_WIDTH_2;
constexpr lv_buttonmatrix_ctrl_t W3 = LV_BUTTONMATRIX_CTRL_WIDTH_3;
constexpr lv_buttonmatrix_ctrl_t W7 = LV_BUTTONMATRIX_CTRL_WIDTH_7;
// Modifiers are NOT CHECKABLE: lv_buttonmatrix toggles CHECKED on RELEASE,
// after the press-time VALUE_CHANGED this handler keys off — so the armed
// state is tracked here and CHECKED is set/cleared manually for the visual.
constexpr lv_buttonmatrix_ctrl_t MOD = LV_BUTTONMATRIX_CTRL_NO_REPEAT;

// One ctrl entry per button ("\n" rows excluded). Ctrl is index 23 in every
// layer (12 + 11 buttons before it) — on_key() relies on that to clear its
// checked state.
constexpr int kCtrlIdx = 23;

const lv_buttonmatrix_ctrl_t kCtrlBase[] = {
    W3, W2, W2, W2, W2, W2, W2, W2, W2, W2, W2, W3,           // Esc row
    W3, W2, W2, W2, W2, W2, W2, W2, W2, W2, W2,               // Tab row
    lv_buttonmatrix_ctrl_t(W3 | MOD), W2, W2, W2, W2, W2, W2, W2, W2, W2, W3,
    lv_buttonmatrix_ctrl_t(W3 | MOD), W2, W2, W2, W2, W2, W2, W2, W2, W2, W2,
    lv_buttonmatrix_ctrl_t(W3 | MOD), W2, W2, W7, W2, W2, W2, W2,
};

const lv_buttonmatrix_ctrl_t kCtrlSym[] = {
    W3, W2, W2, W2, W2, W2, W2, W2, W2, W2, W2, W3,
    W3, W2, W2, W2, W2, W2, W2, W2, W2, W2, W2,
    lv_buttonmatrix_ctrl_t(W3 | MOD), W2, W2, W2, W2, W2, W2, W2, W3,
    lv_buttonmatrix_ctrl_t(W3 | MOD), W3, W3, W3, W3, W3, W3,
    lv_buttonmatrix_ctrl_t(W3 | MOD), W2, W2, W7, W2, W2, W2, W2,
};

}  // namespace

TermKeyboard::TermKeyboard(lv_obj_t *parent, int x, int y, int w, int h) {
    obj_ = lv_buttonmatrix_create(parent);
    lv_obj_set_pos(obj_, x, y);
    lv_obj_set_size(obj_, w, h);
    apply_layer(Layer::Base);

    // Light styling consistent with the FileManager screens.
    lv_obj_set_style_bg_color(obj_, lv_color_hex(0xeeeeee), 0);
    lv_obj_set_style_bg_opa(obj_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj_, 0, 0);
    lv_obj_set_style_radius(obj_, 0, 0);
    lv_obj_set_style_pad_all(obj_, 6, 0);
    lv_obj_set_style_pad_gap(obj_, 6, 0);
    lv_obj_set_style_radius(obj_, 8, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(obj_, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(obj_, lv_color_hex(0xd0d0d0),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(obj_, lv_color_hex(0x202020), LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(obj_, 0, LV_PART_ITEMS);
    // Armed modifier / active layer keys (the all-state white above would
    // otherwise hide the checked state).
    lv_obj_set_style_bg_color(obj_, lv_palette_main(LV_PALETTE_BLUE),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(obj_, lv_color_white(),
                                LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_add_event_cb(obj_, value_changed_cb, LV_EVENT_VALUE_CHANGED, this);
}

TermKeyboard::~TermKeyboard() {
    if (obj_) lv_obj_delete(obj_);
}

void TermKeyboard::set_enabled(bool en) {
    if (en) lv_obj_remove_state(obj_, LV_STATE_DISABLED);
    else lv_obj_add_state(obj_, LV_STATE_DISABLED);
}

void TermKeyboard::apply_layer(Layer l) {
    layer_ = l;
    switch (l) {
        case Layer::Base: lv_buttonmatrix_set_map(obj_, kMapBase); break;
        case Layer::Shift: lv_buttonmatrix_set_map(obj_, kMapShift); break;
        case Layer::Sym: lv_buttonmatrix_set_map(obj_, kMapSym); break;
    }
    lv_buttonmatrix_set_ctrl_map(obj_, l == Layer::Sym ? kCtrlSym : kCtrlBase);
    if (ctrl_armed_)
        lv_buttonmatrix_set_button_ctrl(obj_, kCtrlIdx,
                                        LV_BUTTONMATRIX_CTRL_CHECKED);
    // Show the active layer on its toggle key (button index = position in the
    // map with "\n" rows excluded; Shift is 34 in base-layout maps, "?#&" is
    // 39 in the Sym map whose rows 3/4 are shorter).
    if (l == Layer::Shift)
        lv_buttonmatrix_set_button_ctrl(obj_, 34, LV_BUTTONMATRIX_CTRL_CHECKED);
    if (l == Layer::Sym)
        lv_buttonmatrix_set_button_ctrl(obj_, 39, LV_BUTTONMATRIX_CTRL_CHECKED);
}

void TermKeyboard::value_changed_cb(lv_event_t *e) {
    auto *self = static_cast<TermKeyboard *>(lv_event_get_user_data(e));
    uint32_t id = lv_buttonmatrix_get_selected_button(self->obj_);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *txt = lv_buttonmatrix_get_button_text(self->obj_, id);
    if (txt) self->on_key(txt);
}

void TermKeyboard::send(const uint8_t *data, size_t len) {
    if (sender_) sender_(data, len);
}

void TermKeyboard::send_str(const char *s) {
    send(reinterpret_cast<const uint8_t *>(s), strlen(s));
}

void TermKeyboard::on_key(const char *txt) {
    // --- modifiers / layer switches (no bytes) ---
    if (!strcmp(txt, "Ctrl")) {
        ctrl_armed_ = !ctrl_armed_;
        if (ctrl_armed_)
            lv_buttonmatrix_set_button_ctrl(obj_, kCtrlIdx,
                                            LV_BUTTONMATRIX_CTRL_CHECKED);
        else
            lv_buttonmatrix_clear_button_ctrl(obj_, kCtrlIdx,
                                              LV_BUTTONMATRIX_CTRL_CHECKED);
        return;
    }
    if (!strcmp(txt, "Shift")) {
        apply_layer(layer_ == Layer::Shift ? Layer::Base : Layer::Shift);
        return;
    }
    if (!strcmp(txt, "?#&")) {
        apply_layer(layer_ == Layer::Sym ? Layer::Base : Layer::Sym);
        return;
    }

    // --- special keys ---
    bool app = app_cursor_ && app_cursor_();
    const char *seq = nullptr;
    if (!strcmp(txt, "Esc")) seq = "\x1b";
    else if (!strcmp(txt, "Tab")) seq = "\t";
    else if (!strcmp(txt, LV_SYMBOL_BACKSPACE)) seq = "\x7f";
    else if (!strcmp(txt, LV_SYMBOL_NEW_LINE)) seq = "\r";
    else if (!strcmp(txt, LV_SYMBOL_UP)) seq = app ? "\x1bOA" : "\x1b[A";
    else if (!strcmp(txt, LV_SYMBOL_DOWN)) seq = app ? "\x1bOB" : "\x1b[B";
    else if (!strcmp(txt, LV_SYMBOL_RIGHT)) seq = app ? "\x1bOC" : "\x1b[C";
    else if (!strcmp(txt, LV_SYMBOL_LEFT)) seq = app ? "\x1bOD" : "\x1b[D";
    else if (!strcmp(txt, "Home")) seq = app ? "\x1bOH" : "\x1b[H";
    else if (!strcmp(txt, "End")) seq = app ? "\x1bOF" : "\x1b[F";
    else if (!strcmp(txt, "PgUp")) seq = "\x1b[5~";
    else if (!strcmp(txt, "PgDn")) seq = "\x1b[6~";
    else if (!strcmp(txt, "Ins")) seq = "\x1b[2~";
    else if (!strcmp(txt, "Del")) seq = "\x1b[3~";

    uint8_t byte;
    if (!seq && strlen(txt) == 1) {
        byte = static_cast<uint8_t>(txt[0]);
        if (ctrl_armed_) {
            // Ctrl+letter / Ctrl+@[\]^_ / Ctrl+Space -> control byte
            uint8_t up = (byte >= 'a' && byte <= 'z') ? byte - 0x20 : byte;
            if (byte == ' ') byte = 0;
            else if (up >= '@' && up <= '_') byte = up & 0x1f;
        }
    } else if (!seq) {
        return;  // unknown label
    }

    if (seq) send_str(seq);
    else send(&byte, 1);

    // One-shot modifiers: consume on the first real key.
    if (ctrl_armed_) {
        ctrl_armed_ = false;
        lv_buttonmatrix_clear_button_ctrl(obj_, kCtrlIdx,
                                          LV_BUTTONMATRIX_CTRL_CHECKED);
    }
    if (layer_ == Layer::Shift) apply_layer(Layer::Base);
}
