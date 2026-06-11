#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

#include "lvgl.h"

// TermKeyboard — terminal-oriented on-screen keyboard. A raw lv_buttonmatrix
// (not lv_keyboard, which is textarea-coupled): every key press emits PTY
// bytes through the sender callback, nothing is line-buffered. Three layers
// (base / Shift / symbols), Esc/Tab/arrows/Home..Del on the maps, and a sticky
// one-shot Ctrl (tap Ctrl, then a key -> control byte). Keys auto-repeat on
// long press (buttonmatrix LONG_PRESSED_REPEAT -> VALUE_CHANGED); modifiers
// don't. Arrows/Home/End honour the emulator's DECCKM via the app_cursor
// query (CSI vs SS3 encoding).
class TermKeyboard {
public:
    using Sender = std::function<void(const uint8_t *data, size_t len)>;
    using AppCursorQuery = std::function<bool()>;

    TermKeyboard(lv_obj_t *parent, int x, int y, int w, int h);
    ~TermKeyboard();

    lv_obj_t *obj() const { return obj_; }
    void set_sender(Sender s) { sender_ = std::move(s); }
    void set_app_cursor_query(AppCursorQuery q) { app_cursor_ = std::move(q); }
    void set_enabled(bool en);  // disable on shell close

private:
    enum class Layer { Base, Shift, Sym };

    static void value_changed_cb(lv_event_t *e);
    void on_key(const char *txt);
    void apply_layer(Layer l);
    void send(const uint8_t *data, size_t len);
    void send_str(const char *s);

    lv_obj_t *obj_ = nullptr;
    Sender sender_;
    AppCursorQuery app_cursor_;
    Layer layer_ = Layer::Base;
    bool ctrl_armed_ = false;
};
