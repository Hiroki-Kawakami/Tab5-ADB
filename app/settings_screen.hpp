#pragma once
#include "screen.hpp"

// App settings screen. The body is intentionally empty for now — settings rows
// land later; this gives the nav chrome and the HomeScreen entry point.
class SettingsScreen : public Screen {
public:
    // wifi_enabled: whether the Wi-Fi settings entry is tappable. Disabled
    // (greyed, non-clickable) when opened during an active ADB session
    // (ADBDeviceScreen) — changing Wi-Fi mid-link would disrupt the connection.
    explicit SettingsScreen(bool wifi_enabled = true) : wifi_enabled_(wifi_enabled) {}

    void build() override;

private:
    bool wifi_enabled_;
    lv_obj_t *body_ = nullptr;
};
