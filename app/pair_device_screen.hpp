#pragma once

#include "screen.hpp"
#include "wifi_manager.hpp"

class PairDeviceScreen : public Screen, public wifi::Listener {
public:
    void build() override;
    void onEnter() override;
    void onAppear() override;

    void on_wifi_state(const wifi::Status &status) override;

private:
    lv_obj_t *address_ = nullptr;
    lv_obj_t *code_ = nullptr;
    lv_obj_t *pair_button_ = nullptr;
    lv_obj_t *wifi_status_ = nullptr;
    lv_obj_t *result_label_ = nullptr;
    lv_obj_t *keyboard_ = nullptr;
    lv_obj_t *keyboard_target_ = nullptr;
    lv_obj_t *progress_card_ = nullptr;
    bool busy_ = false;

    void show_keyboard(lv_obj_t *textarea, bool code_mode);
    void hide_keyboard();
    void start_pairing();
    void update_controls();
    void open_progress();
    void close_progress();
};
