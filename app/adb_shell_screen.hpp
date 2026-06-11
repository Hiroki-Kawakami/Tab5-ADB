#pragma once
#include <memory>
#include <mutex>
#include <string>

#include "adb.hpp"  // adb::Shell, adb::ShellListener, adb::Error
#include "screen.hpp"
#include "term_emu.hpp"
#include "terminal/term_keyboard.hpp"
#include "terminal/term_view.hpp"

// Interactive ADB terminal: an interactive `shell:` PTY rendered through a
// VT100/xterm-subset emulator (term_emu) into a custom cell-grid widget
// (TermView), with a terminal-oriented on-screen keyboard (TermKeyboard) that
// sends key bytes straight to the PTY — no input line, the PTY's echo is the
// feedback. adb shell v1 has no window-size channel, so build() bootstraps the
// PTY with `stty rows/columns` + TERM (the echoed line is wiped by the
// trailing `clear`).
//
// The screen IS the adb::ShellListener. Shell callbacks fire on the reader
// thread; PTY bytes are buffered FIFO under out_mtx_ and one coalesced
// lv_async_call drains them into the emulator ON THE LVGL THREAD (the
// emulator is single-threaded by design). Each marshalling lambda captures
// `self = shared_from_this()` and skips when Screen::exited() is set, so
// updates queued before teardown never touch freed widgets.
class ADBShellScreen : public Screen, public adb::ShellListener {
public:
    ADBShellScreen();
    ~ADBShellScreen() override;

    void build() override;
    void onExit() override;  // close() the shell before destruction

    // adb::ShellListener — both fire on the reader thread.
    void on_shell_data(adb::Shell *sh, const uint8_t *data, size_t len) override;
    void on_shell_close(adb::Shell *sh, adb::Error err) override;

private:
    void flush_output();  // LVGL thread: drain pending_out_ into the emulator

    std::shared_ptr<adb::Shell> shell_;
    std::unique_ptr<term::TermEmu> emu_;
    std::unique_ptr<TermView> view_;
    std::unique_ptr<TermKeyboard> keyboard_;

    // Output ordering: lv_async_call runs queued callbacks LIFO within one
    // timer pass, so per-chunk async appends would render reversed. The reader
    // thread accumulates into pending_out_ (FIFO) under out_mtx_ and coalesces
    // a single flush. The buffer is capped: past the cap incoming bytes are
    // dropped (overflow_) and the flush re-grounds the parser + injects an
    // "[output dropped]" marker, so a stalled LVGL thread can't OOM us and a
    // mid-escape drop can't corrupt the FSM.
    std::mutex out_mtx_;
    std::string pending_out_;
    bool flush_scheduled_ = false;
    bool overflow_ = false;
    bool close_marker_ = false;  // shell closed: disable the keyboard at flush
};
