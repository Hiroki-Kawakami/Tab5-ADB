#include "settings.hpp"

#include <nvs.h>
#include <nvs_flash.h>

namespace app {
namespace {

constexpr const char* kNamespace = "tab5adb";
constexpr const char* kAudioModeKey = "audio_mode";

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

}  // namespace app
