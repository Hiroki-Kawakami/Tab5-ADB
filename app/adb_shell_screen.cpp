#include "adb_shell_screen.hpp"

#include <cstdio>
#include <cstring>

#include "adb_app.hpp"
#include "lvgl.hpp"
#include "screen_manager.hpp"

namespace {

// Layout (portrait 720x1280): FileManager-style nav bar, full-bleed terminal,
// keyboard. The terminal area divides exactly into 10x17px hack_16 cells
// (72 cols x 42 rows).
constexpr int kNavH = 120;
constexpr int kKeyboardH = 440;
constexpr int kTermH = PANEL_H - kNavH - kKeyboardH;

// Reader-thread buffer cap (see the header). A `cat` burst parses far faster
// than USB delivers, so this only trips if the LVGL thread stalls.
constexpr size_t kPendingCap = 256 * 1024;

}  // namespace

ADBShellScreen::ADBShellScreen() = default;

ADBShellScreen::~ADBShellScreen() {
    // onExit normally runs first (pop()/load()), but guard against a destroy
    // without it: close() stops I/O. The shell holds the listener weakly, so
    // the weak ref simply expires as this screen dies.
    if (shell_) {
        shell_->close();
        shell_.reset();
    }
}

void ADBShellScreen::build() {
    lv_obj_set_size(root_, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0xeeeeee), 0);
    lv_obj_set_style_pad_all(root_, 0, 0);

    // --- nav bar: Back + title, consistent with ADBFileManagerScreen ---
    auto navigation = lv_obj_create(root_);
    lv_obj_remove_style_all(navigation);
    lv_obj_set_size(navigation, PANEL_W, kNavH);
    lv_obj_set_pos(navigation, 0, 0);
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

    auto back = lv_button_create(navigation);
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
    auto back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_38, 0);
    lv_obj_set_style_pad_all(back_label, 8, 0);
    auto title = lv_label_create(back);
    lv_label_set_text(title, "Terminal");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_38, 0);

    // --- terminal: emulator grid sized to the area's cell capacity ---
    term::TermEmu::Config cfg;
    cfg.cols = PANEL_W / TermView::cell_w();
    cfg.rows = kTermH / TermView::cell_h();
    emu_ = std::make_unique<term::TermEmu>(cfg);
    view_ = std::make_unique<TermView>(root_, emu_.get(), 0, kNavH, PANEL_W,
                                       kTermH);

    // DSR/DA responses go back to the PTY. The responder fires inside feed(),
    // i.e. on the LVGL thread; Shell::write is non-blocking from any thread.
    emu_->set_responder([this](const uint8_t *data, size_t len) {
        if (shell_) shell_->write(data, len);
    });

    // --- keyboard: key bytes straight to the PTY ---
    keyboard_ = std::make_unique<TermKeyboard>(root_, 0, kNavH + kTermH,
                                               PANEL_W, kKeyboardH);
    keyboard_->set_app_cursor_query([this] { return emu_->app_cursor_keys(); });
    keyboard_->set_sender([this](const uint8_t *data, size_t len) {
        if (!shell_) return;
        view_->snap_to_live();          // typing jumps out of scrollback
        shell_->write(data, len);       // QueueFull -> the key is dropped
    });

    // --- open the shell (the screen is its listener) ---
    // The shell holds the listener weakly. Hand it a shared_ptr aliasing this
    // screen's control block (via shared_from_this) so the weak ref expires
    // when the screen is freed.
    adb::Client *client = app::adb_client();
    std::shared_ptr<adb::ShellListener> self(
        shared_from_this(), static_cast<adb::ShellListener *>(this));
    shell_ = client ? client->open_shell(self) : nullptr;
    if (!shell_) {
        emu_->feed("(not connected)\r\n");
        view_->refresh();
        keyboard_->set_enabled(false);
        return;
    }

    // Bootstrap the PTY: v1 `shell:` has no window-size/TERM channel, so set
    // both in-band (pre-open writes queue until the stream opens). The echoed
    // command line is wiped by the trailing `clear`.
    char boot[96];
    snprintf(boot, sizeof(boot),
             "stty rows %d columns %d 2>/dev/null; "
             "export TERM=xterm-256color; clear\n",
             emu_->rows(), emu_->cols());
    shell_->write(boot);
}

void ADBShellScreen::onExit() {
    // Runs on the LVGL thread before the screen object is destroyed (exited()
    // is already set). close() stops the shell I/O; the shell's weak listener
    // ref expires when this screen is freed, so no callback outlives `this`.
    // Any update already marshalled sees exited() and skips the freed widgets.
    if (shell_) {
        shell_->close();
        shell_.reset();
    }
}

void ADBShellScreen::flush_output() {
    std::string chunk;
    bool overflow, close_marker;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        chunk.swap(pending_out_);
        overflow = overflow_;
        close_marker = close_marker_;
        overflow_ = false;
        close_marker_ = false;
        flush_scheduled_ = false;
    }
    if (!chunk.empty())
        emu_->feed(reinterpret_cast<const uint8_t *>(chunk.data()),
                   chunk.size());
    if (overflow) {
        // Bytes after the kept chunk were dropped: a partial escape sequence
        // may be in flight, so CAN re-grounds the parser before the marker.
        emu_->feed("\x18\r\n[output dropped]\r\n");
    }
    if (close_marker) {
        emu_->feed("\r\n[shell closed]\r\n");
        keyboard_->set_enabled(false);
    }
    view_->refresh();
}

void ADBShellScreen::on_shell_data(adb::Shell * /*sh*/, const uint8_t *data,
                                   size_t len) {
    // Reader thread: buffer in arrival order and schedule one coalesced flush.
    bool need_schedule;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        if (overflow_ || pending_out_.size() + len > kPendingCap) {
            overflow_ = true;  // drop until the next flush drains the buffer
        } else {
            pending_out_.append(reinterpret_cast<const char *>(data), len);
        }
        need_schedule = !flush_scheduled_;  // coalesce: one flush per burst
        flush_scheduled_ = true;
    }
    if (!need_schedule) return;  // a flush is already queued; it'll see this

    // Strong `self` keeps the screen alive until this runs on the LVGL thread,
    // where the last ref drops; exited() skips the freed widgets after
    // teardown.
    lv_async_call([self = shared_from_this(), this]() {
        if (exited()) return;
        flush_output();
    });
}

void ADBShellScreen::on_shell_close(adb::Shell * /*sh*/, adb::Error /*err*/) {
    // Deliver the marker through the same FIFO so it lands after the last
    // output; the flush also disables the keyboard on the LVGL thread.
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        close_marker_ = true;
        flush_scheduled_ = true;
    }
    lv_async_call([self = shared_from_this(), this]() {
        if (exited()) return;
        flush_output();
    });
}
