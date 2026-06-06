#pragma once
#include <string>
#include "esp_err.h"
#include "nvs_flash.h"

class NVS {
public:
    enum class Error {
        OK = 0,
        NotFound,
        Failed,
    };

    enum class OpenMode {
        ReadWrite = NVS_READWRITE,
        ReadOnly  = NVS_READONLY,
    };

    static Error init() {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        return toError(ret);
    }

    NVS(const char *name, OpenMode open_mode = OpenMode::ReadWrite) : name_(name), open_mode_(open_mode) {}
    ~NVS() {
        nvs_close(handle_);
    }

    Error erase(std::string key) { return toError(nvs_erase_key(open(), key.c_str())); }
    Error eraseAll() { return toError(nvs_erase_all(open())); }
    Error commit() { return toError(nvs_commit(open())); }
    Error getUsedEntryCount(size_t *used_entries) { return toError(nvs_get_used_entry_count(open(), used_entries)); }

    // set functions
    Error set(std::string key, int8_t value) { return toError(nvs_set_i8(open(), key.c_str(), value)); }
    Error set(std::string key, uint8_t value) { return toError(nvs_set_u8(open(), key.c_str(), value)); }
    Error set(std::string key, int16_t value) { return toError(nvs_set_i16(open(), key.c_str(), value)); }
    Error set(std::string key, uint16_t value) { return toError(nvs_set_u16(open(), key.c_str(), value)); }
    Error set(std::string key, int32_t value) { return toError(nvs_set_i32(open(), key.c_str(), value)); }
    Error set(std::string key, uint32_t value) { return toError(nvs_set_u32(open(), key.c_str(), value)); }
    Error set(std::string key, int64_t value) { return toError(nvs_set_i64(open(), key.c_str(), value)); }
    Error set(std::string key, uint64_t value) { return toError(nvs_set_u64(open(), key.c_str(), value)); }
    Error set(std::string key, const char *value) { return toError(nvs_set_str(open(), key.c_str(), value)); }
    Error set(std::string key, const void *value, size_t length) { return toError(nvs_set_blob(open(), key.c_str(), value, length)); }

    // get functions
    Error get(std::string key, int8_t *out_value) { return toError(nvs_get_i8(open(), key.c_str(), out_value)); }
    Error get(std::string key, uint8_t *out_value) { return toError(nvs_get_u8(open(), key.c_str(), out_value)); }
    Error get(std::string key, int16_t *out_value) { return toError(nvs_get_i16(open(), key.c_str(), out_value)); }
    Error get(std::string key, uint16_t *out_value) { return toError(nvs_get_u16(open(), key.c_str(), out_value)); }
    Error get(std::string key, int32_t *out_value) { return toError(nvs_get_i32(open(), key.c_str(), out_value)); }
    Error get(std::string key, uint32_t *out_value) { return toError(nvs_get_u32(open(), key.c_str(), out_value)); }
    Error get(std::string key, int64_t *out_value) { return toError(nvs_get_i64(open(), key.c_str(), out_value)); }
    Error get(std::string key, uint64_t *out_value) { return toError(nvs_get_u64(open(), key.c_str(), out_value)); }
    Error get(std::string key, char *out_value, size_t *length) { return toError(nvs_get_str(open(), key.c_str(), out_value, length)); }
    Error get(std::string key, void *out_value, size_t *length) { return toError(nvs_get_blob(open(), key.c_str(), out_value, length)); }

private:
    nvs_handle_t handle_ = 0;
    const char *name_;
    OpenMode open_mode_;

    nvs_handle_t open() {
        if (!handle_) ESP_ERROR_CHECK(nvs_open(name_, static_cast<nvs_open_mode_t>(open_mode_), &handle_));
        return handle_;
    }

    static Error toError(esp_err_t err) {
        switch (err) {
        case ESP_OK:                return Error::OK;
        case ESP_ERR_NVS_NOT_FOUND: return Error::NotFound;
        default:                    return Error::Failed;
        }
    }
};
