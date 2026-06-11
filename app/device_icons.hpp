#pragma once
// Lucide glyph + color mapping for the devinfo structs — shared by the
// ADBDeviceScreen summary header and ADBDeviceInfoScreen's network/battery
// cards. UI-side companion of device_info.hpp (which stays LVGL-free so it can
// be host-unit-tested).

#include "device_info.hpp"
#include "lvgl.h"
#include "resources/resources.h"

namespace app::devinfo {

inline const char *battery_icon(const Battery &b) {
    if (b.powered) return LUCIDE_BATTERY_CHARGING;
    if (b.level >= 85) return LUCIDE_BATTERY_FULL;
    if (b.level >= 50) return LUCIDE_BATTERY_MEDIUM;
    if (b.level >= 20) return LUCIDE_BATTERY_LOW;
    return LUCIDE_BATTERY_WARNING;
}

// Icon color carrying the at-a-glance state: blue while actively charging,
// red when critically low, default otherwise.
inline lv_color_t battery_color(const Battery &b) {
    if (b.powered && b.charging()) return lv_palette_main(LV_PALETTE_BLUE);
    if (!b.powered && b.level >= 0 && b.level < 20) return lv_color_hex(0xd32f2f);
    return lv_color_hex(0x404040);
}

inline const char *wifi_icon(const Wifi &w) {
    if (w.state != Wifi::State::Connected) return LUCIDE_WIFI_OFF;
    switch (w.bars()) {
        case 4: return LUCIDE_WIFI;
        case 3: return LUCIDE_WIFI_HIGH;
        case 2: return LUCIDE_WIFI_LOW;
        default: return LUCIDE_WIFI_ZERO;
    }
}

inline const char *cellular_icon(const Cellular &c) {
    if (!c.in_service) return LUCIDE_SIGNAL_ZERO;
    switch (c.level) {
        case 4: return LUCIDE_SIGNAL;
        case 3: return LUCIDE_SIGNAL_HIGH;
        case 2: return LUCIDE_SIGNAL_MEDIUM;
        case 1: return LUCIDE_SIGNAL_LOW;
        default: return LUCIDE_SIGNAL_ZERO;
    }
}

inline lv_color_t icon_active_color() { return lv_color_hex(0x404040); }
inline lv_color_t icon_inactive_color() { return lv_color_hex(0xbbbbbb); }

}  // namespace app::devinfo
