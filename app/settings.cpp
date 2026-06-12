#include "settings.hpp"

#include <nvs.h>
#include <nvs_flash.h>

namespace app {
namespace {

constexpr const char* kNamespace = "tab5adb";
constexpr const char* kAudioModeKey = "audio_mode";
constexpr const char* kBrightnessKey = "brightness";
constexpr const char* kColorDepthKey = "color_depth";

// nvs_flash is initialised at boot (the BSP / the adb keystore), but a settings
// access could in principle precede those, so ensure it once here too. Idempotent
// — nvs_flash_init returns ESP_OK if already initialised.
void ensure_nvs() {
    static bool done = false;
    if (done) return;
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    done = true;
}

}  // namespace

AudioOutputMode audio_output_mode() {
    ensure_nvs();
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK)
        return AudioOutputMode::Tab5Only;  // no namespace yet = default
    uint8_t v = 0;  // missing key -> 0 -> Tab5Only
    nvs_get_u8(h, kAudioModeKey, &v);
    nvs_close(h);
    return v == static_cast<uint8_t>(AudioOutputMode::PhoneOnly)
               ? AudioOutputMode::PhoneOnly
               : AudioOutputMode::Tab5Only;
}

void set_audio_output_mode(AudioOutputMode mode) {
    ensure_nvs();
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, kAudioModeKey, static_cast<uint8_t>(mode));
    nvs_commit(h);
    nvs_close(h);
}

int display_brightness() {
    ensure_nvs();
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return 80;
    uint8_t v = 80;  // default, also covers a missing key (get leaves v untouched)
    nvs_get_u8(h, kBrightnessKey, &v);
    nvs_close(h);
    if (v < 1) v = 1;  // 0 fully blanks the backlight; keep at least the minimum
    if (v > 100) v = 100;
    return v;
}

void set_display_brightness(int level) {
    ensure_nvs();
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, kBrightnessKey, static_cast<uint8_t>(level));
    nvs_commit(h);
    nvs_close(h);
}

ColorDepth display_color_depth() {
    ensure_nvs();
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK)
        return ColorDepth::Color24;  // no namespace yet = default
    uint8_t v = 0;  // missing key -> 0 -> Color24
    nvs_get_u8(h, kColorDepthKey, &v);
    nvs_close(h);
    return v == static_cast<uint8_t>(ColorDepth::Color16) ? ColorDepth::Color16
                                                          : ColorDepth::Color24;
}

void set_display_color_depth(ColorDepth depth) {
    ensure_nvs();
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, kColorDepthKey, static_cast<uint8_t>(depth));
    nvs_commit(h);
    nvs_close(h);
}

}  // namespace app
