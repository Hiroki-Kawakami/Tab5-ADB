#include "adb_keystore.hpp"

#include <nvs.h>
#include <nvs_flash.h>

#include <vector>

namespace adb {

namespace {
constexpr char kKeyName[] = "privkey";

// Ensure the NVS subsystem is up. Idempotent; on device, recover from a layout
// change / full partition by erasing once (the standard ESP-IDF pattern). On the
// simulator nvs_flash_init just loads the backing JSON.
bool ensure_nvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() != ESP_OK) return false;
        err = nvs_flash_init();
    }
    return err == ESP_OK;
}
}  // namespace

std::optional<RsaKey> load_or_create_key(const char* nvs_namespace) {
    if (!ensure_nvs()) return std::nullopt;

    nvs_handle_t handle;
    if (nvs_open(nvs_namespace, NVS_READWRITE, &handle) != ESP_OK) {
        return std::nullopt;
    }

    std::optional<RsaKey> result;
    // Try to load an existing key.
    size_t len = 0;
    if (nvs_get_blob(handle, kKeyName, nullptr, &len) == ESP_OK && len > 0) {
        std::vector<uint8_t> der(len);
        if (nvs_get_blob(handle, kKeyName, der.data(), &len) == ESP_OK) {
            result = RsaKey::from_der(der.data(), len);
        }
    }

    // First run (or a corrupt/unreadable stored key): generate and persist.
    if (!result) {
        result = RsaKey::generate();
        std::vector<uint8_t> der;
        if (result && result->to_der(der)) {
            if (nvs_set_blob(handle, kKeyName, der.data(), der.size()) != ESP_OK ||
                nvs_commit(handle) != ESP_OK) {
                result = std::nullopt;  // couldn't persist — surface the failure
            }
        } else {
            result = std::nullopt;
        }
    }

    nvs_close(handle);
    return result;
}

}  // namespace adb
