#pragma once
#include "screen.hpp"

class HomeScreen : public Screen {
public:
    void build() override;
    void onAppear() override;   // refresh the live Wi-Fi status line

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
