#pragma once
#include <memory>
#include <mutex>
#include <string>

#include "adb.hpp"  // adb::Shell, adb::ShellListener, adb::Error
#include "screen.hpp"

// Interactive ADB terminal. Opens an interactive `shell:` session on the app's
// adb::Client and bridges it to an LVGL UI: device output streams into a
// read-only monospace text area, and an input field + on-screen keyboard sends
// lines to the shell.
//
// The screen IS the adb::ShellListener. Shell callbacks fire on the reader
// thread; this marshals every UI update to the LVGL thread with lv_async_call.
// Each marshalling lambda captures `self = shared_from_this()` (keeping the
// screen alive until it drains on the LVGL thread) and skips the update when the
// base Screen::exited() flag is set — so updates queued before teardown never
// touch freed widgets.
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
    void send_input();          // LVGL thread: write the input line to the shell
    void flush_output();        // LVGL thread: drain pending_out_ into the textarea
    void append_output(const std::string &text);  // LVGL thread: append + scroll

    std::shared_ptr<adb::Shell> shell_;

    // Output ordering: lv_async_call runs queued callbacks LIFO within one timer
    // pass, so per-chunk async appends would render reversed. Instead the reader
    // thread accumulates into pending_out_ (FIFO) under out_mtx_ and coalesces a
    // single flush; the flush drains the whole buffer in order on the LVGL thread.
    std::mutex out_mtx_;
    std::string pending_out_;
    bool flush_scheduled_ = false;

    lv_obj_t *output_ = nullptr;    // read-only terminal text area
    lv_obj_t *input_ = nullptr;     // single-line input
    lv_obj_t *keyboard_ = nullptr;  // on-screen keyboard
};
