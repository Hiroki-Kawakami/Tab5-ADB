#pragma once
#include <string>
#include <vector>

#include "screen.hpp"
#include "wifi_manager.hpp"

// Wi-Fi setup screen: scan nearby APs, connect (with a password modal for secured
// networks), and show live connection status. Reached from the HomeScreen
// Wireless card. Gets the Tab5 onto a LAN — the prerequisite for ADB-over-TCP.
//
// The screen IS the wifi::Listener (held weakly by wifi::manager()); all wifi
// callbacks fire on the event thread and are marshalled to LVGL with
// lv_async_call + an exited() guard (the standard app pattern).
class WiFiScreen : public Screen, public wifi::Listener {
public:
    void build() override;
    void onEnter() override;
    void onAppear() override;

    // wifi::Listener — event thread; marshalled to LVGL.
    void on_wifi_state(const wifi::Status& status) override;

private:
    lv_obj_t *enable_switch_ = nullptr; // Wi-Fi on/off
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *list_ = nullptr;          // AP rows container
    lv_obj_t *spinner_ = nullptr;       // shown while scanning
    lv_obj_t *pw_modal_ = nullptr;      // password entry dialog (card only)
    lv_obj_t *pw_keyboard_ = nullptr;   // its keyboard — anchored to the screen
                                        // bottom, OUTSIDE the modal card
    lv_obj_t *connecting_modal_ = nullptr;

    std::vector<wifi::AP> aps_;
    int scan_gen_ = 0;                  // drops stale scan completions
    bool busy_ = false;                 // a bring-up/teardown task is in flight
                                        // (LVGL thread only); switch is disabled

    void start_scan();
    void rebuild_list();
    void update_status(const wifi::Status &s);
    void set_enabled(bool on);          // toggle the radio via the manager worker
    void select_ap(const std::string &ssid, bool secured);
    void open_password_modal(const std::string &ssid);
    void close_password_modal();
    void do_connect(const std::string &ssid, const std::string &password);
};
