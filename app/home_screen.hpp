#pragma once
#include <string>

#include "screen.hpp"
#include "wifi_manager.hpp"

// HomeScreen registers as the wifi::Listener while visible (held weakly, no
// bring-up) so the Wireless card's status line updates live — e.g. the boot
// background auto-connect completing. Callbacks fire on the wifi event thread and
// are marshalled to LVGL.
class HomeScreen : public Screen, public wifi::Listener {
public:
    void build() override;
    void onAppear() override;   // re-register the listener + refresh the status line

    // wifi::Listener — event thread; marshalled to LVGL.
    void on_wifi_state(const wifi::Status& status) override;

private:
    lv_obj_t *connect_btn_ = nullptr;
    lv_obj_t *progress_card_ = nullptr;   // connect-progress modal; null when closed
    lv_obj_t *progress_label_ = nullptr;  // its message line
    lv_obj_t *wifi_status_ = nullptr;     // Wireless card status line
    lv_obj_t *tcp_addr_ = nullptr;        // Wireless card target input (host:port)
    lv_obj_t *tcp_history_box_ = nullptr; // recent-targets list; placeholder when empty
    lv_obj_t *tcp_keyboard_ = nullptr;    // on-screen keyboard for the address; null when hidden

    void build_hero();
    void build_body();
    void start_connect();                 // USB
    void start_connect_tcp(const std::string &addr);  // ADB-over-TCP to host:port
    // Shared post-adb continuation (agent bring-up -> push device screen, or error).
    void proceed_after_connect(bool ok, const char *fail_msg);
    void refresh_wifi_status();

    // (Re)populate the recent ADB-over-TCP targets list from NVS. Called on build
    // and onAppear (a connect may have added an entry). Shows a placeholder when
    // empty.
    void rebuild_tcp_history();
    // Pick a recent target: fill the address field and connect to it.
    void select_tcp_target(const std::string &target);
    // The address textarea's on-screen keyboard (pinned to the screen bottom).
    void show_tcp_keyboard();
    void hide_tcp_keyboard();

    // Connect-progress modal (spinner + message). The scrim blocks taps to the
    // cards behind, so a connect in flight can't be interrupted by navigating
    // away; close it before pushing the device screen or showing an error.
    void open_progress(const char *message);
    void set_progress(const char *message);
    void close_progress();
};
