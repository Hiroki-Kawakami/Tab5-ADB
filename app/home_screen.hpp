#pragma once
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

    void build_hero();
    void build_body();
    void start_connect();
    void refresh_wifi_status();

    // Connect-progress modal (spinner + message). The scrim blocks taps to the
    // cards behind, so a connect in flight can't be interrupted by navigating
    // away; close it before pushing the device screen or showing an error.
    void open_progress(const char *message);
    void set_progress(const char *message);
    void close_progress();
};
