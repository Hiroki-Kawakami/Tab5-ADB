#include "adb_shell_screen.hpp"

#include <cstdint>
#include <cstring>
#include <string>

#include "adb_app.hpp"
#include "lvgl.hpp"
#include "screen_manager.hpp"

namespace {

// Output is rendered with a monospace bitmap font (UNSCII), so strip control
// bytes and ANSI/VT escape sequences the text area can't display — otherwise a
// PTY shell's color codes and CR/escape noise show up as garbage glyphs.
std::string sanitize(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0x1b) {  // ESC: skip the escape sequence
            if (i + 1 < in.size() && in[i + 1] == '[') {
                i += 2;  // CSI: ESC '[' params... final byte in 0x40..0x7e
                while (i < in.size()) {
                    unsigned char d = static_cast<unsigned char>(in[i]);
                    if (d >= 0x40 && d <= 0x7e) break;
                    ++i;
                }
            } else {
                ++i;  // other ESC x: drop the one following byte
            }
            continue;
        }
        if (c == '\n' || c == '\t') { out += static_cast<char>(c); continue; }
        if (c == '\r') continue;                 // drop CR (textarea uses \n)
        if (c < 0x20 || c >= 0x7f) continue;     // drop remaining control / non-ASCII
        out += static_cast<char>(c);
    }
    return out;
}

// Keep the on-screen scrollback bounded: trimming to a tail keeps the text area
// fast (re-laying out a huge buffer on every chunk is expensive).
constexpr size_t kMaxChars = 8000;
constexpr size_t kKeepChars = 6000;

// Layout (portrait 720x1280): top bar, output, input, keyboard stacked.
constexpr int kTopH = 64;
constexpr int kInputH = 56;
constexpr int kKeyboardH = 440;

}  // namespace

ADBShellScreen::ADBShellScreen() = default;

ADBShellScreen::~ADBShellScreen() {
    // onExit normally runs first (pop()/load()), but guard against a destroy
    // without it: close() stops I/O. The shell holds the listener weakly, so the
    // weak ref simply expires as this screen dies.
    if (shell_) {
        shell_->close();
        shell_.reset();
    }
}

void ADBShellScreen::build() {
    lv_obj_set_size(root_, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(root_, 0, 0);

    // --- top bar: Back button + title ---
    lv_obj_t *bar = lv_obj_create(root_);
    lv_obj_set_size(bar, PANEL_W, kTopH);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1c2228), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 8, 0);

    lv_obj_t *back = lv_button_create(bar);
    lv_obj_set_size(back, 96, kTopH - 16);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_lbl);
    lv_obj_add_event_fn(back, LV_EVENT_CLICKED,
                        [](lv_event_t *) { screen_manager.pop(); });

    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "ADB Shell");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    // --- output: read-only monospace terminal ---
    output_ = lv_textarea_create(root_);
    lv_obj_set_pos(output_, 0, kTopH);
    lv_obj_set_size(output_, PANEL_W, PANEL_H - kTopH - kInputH - kKeyboardH);
    lv_textarea_set_text(output_, "");
    lv_textarea_set_cursor_click_pos(output_, false);  // don't move cursor on tap
    lv_obj_set_style_text_font(output_, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(output_, lv_color_hex(0xd0f0d0), 0);
    lv_obj_set_style_bg_color(output_, lv_color_hex(0x0a0d10), 0);
    lv_obj_set_style_radius(output_, 0, 0);

    // --- input: single line, fed by the keyboard ---
    input_ = lv_textarea_create(root_);
    lv_obj_set_pos(input_, 0, PANEL_H - kInputH - kKeyboardH);
    lv_obj_set_size(input_, PANEL_W, kInputH);
    lv_textarea_set_one_line(input_, true);
    lv_textarea_set_placeholder_text(input_, "type a command...");
    lv_obj_set_style_text_font(input_, &lv_font_unscii_16, 0);
    lv_obj_set_style_radius(input_, 0, 0);

    // --- keyboard ---
    // lv_keyboard's constructor aligns itself BOTTOM_MID, so set the size and
    // re-affirm the bottom alignment (using lv_obj_set_pos here would treat the
    // y as an offset from BOTTOM_MID and push the keyboard off-screen).
    keyboard_ = lv_keyboard_create(root_);
    lv_obj_set_size(keyboard_, PANEL_W, kKeyboardH);
    lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard_, input_);
    // The OK / checkmark key fires LV_EVENT_READY on the keyboard: send the line.
    lv_obj_add_event_fn(keyboard_, LV_EVENT_READY,
                        [this](lv_event_t *) { send_input(); });

    // --- open the shell (the screen is its listener) ---
    // The shell holds the listener weakly. Hand it a shared_ptr aliasing this
    // screen's control block (via shared_from_this) so the weak ref expires when
    // the screen is freed.
    adb::Client *client = app::adb_client();
    std::shared_ptr<adb::ShellListener> self(
        shared_from_this(), static_cast<adb::ShellListener *>(this));
    shell_ = client ? client->open_shell(self) : nullptr;
    if (!shell_) {
        append_output("(not connected)\n");
        lv_obj_add_state(input_, LV_STATE_DISABLED);
    }
}

void ADBShellScreen::onExit() {
    // Runs on the LVGL thread before the screen object is destroyed (exited() is
    // already set). close() stops the shell I/O; the shell's weak listener ref
    // expires when this screen is freed, so no callback outlives `this`. Any
    // update already marshalled sees exited() and skips the freed widgets.
    if (shell_) {
        shell_->close();
        shell_.reset();
    }
}

void ADBShellScreen::send_input() {
    if (!shell_ || !input_) return;
    const char *t = lv_textarea_get_text(input_);
    std::string line = t ? t : "";
    line += "\n";
    shell_->write(line);  // non-blocking; PTY echoes the text back into output_
    lv_textarea_set_text(input_, "");
}

void ADBShellScreen::flush_output() {
    std::string chunk;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        chunk.swap(pending_out_);
        flush_scheduled_ = false;
    }
    if (!chunk.empty()) append_output(chunk);
}

void ADBShellScreen::append_output(const std::string &text) {
    if (!output_) return;
    lv_textarea_add_text(output_, text.c_str());

    const char *cur = lv_textarea_get_text(output_);
    size_t len = cur ? std::strlen(cur) : 0;
    if (len > kMaxChars) {
        std::string tail(cur + (len - kKeepChars));
        lv_textarea_set_text(output_, tail.c_str());
    }
    lv_textarea_set_cursor_pos(output_, LV_TEXTAREA_CURSOR_LAST);  // scroll to end
}

void ADBShellScreen::on_shell_data(adb::Shell * /*sh*/, const uint8_t *data,
                                   size_t len) {
    // Reader thread: sanitize (cheap), buffer in arrival order, and schedule one
    // coalesced flush. Buffering preserves order despite lv_async_call's LIFO.
    std::string text = sanitize(
        std::string(reinterpret_cast<const char *>(data), len));
    if (text.empty()) return;

    bool need_schedule;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        pending_out_ += text;
        need_schedule = !flush_scheduled_;  // coalesce: one flush drains a burst
        flush_scheduled_ = true;
    }
    if (!need_schedule) return;  // a flush is already queued; it'll see this data

    // Strong `self` keeps the screen alive until this runs on the LVGL thread,
    // where the last ref drops; exited() skips the freed widgets after teardown.
    lv_async_call([self = shared_from_this(), this]() {
        if (exited()) return;
        flush_output();
    });
}

void ADBShellScreen::on_shell_close(adb::Shell * /*sh*/, adb::Error /*err*/) {
    // Append the marker through the same FIFO buffer so it lands after the last
    // output, then flush + disable input on the LVGL thread.
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        pending_out_ += "\n[shell closed]\n";
        flush_scheduled_ = true;
    }
    lv_async_call([self = shared_from_this(), this]() {
        if (exited()) return;
        flush_output();
        if (input_) lv_obj_add_state(input_, LV_STATE_DISABLED);
    });
}
