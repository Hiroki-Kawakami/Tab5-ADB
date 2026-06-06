#pragma once
// Host counterpart of ESP-IDF <esp_check.h>: the ESP_RETURN_ON_* / ESP_GOTO_ON_*
// error-propagation macros used throughout IDF-style C code. Each expects a
// `TAG` (or some string) for the log line, matching the on-device signatures.
#include "esp_err.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_RETURN_ON_ERROR(x, log_tag, format, ...)                          \
    do {                                                                      \
        esp_err_t err_rc_ = (x);                                              \
        if (err_rc_ != ESP_OK) {                                             \
            ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__,      \
                     ##__VA_ARGS__);                                          \
            return err_rc_;                                                   \
        }                                                                     \
    } while (0)

#define ESP_GOTO_ON_ERROR(x, goto_tag, log_tag, format, ...)                  \
    do {                                                                      \
        esp_err_t err_rc_ = (x);                                              \
        if (err_rc_ != ESP_OK) {                                             \
            ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__,      \
                     ##__VA_ARGS__);                                          \
            ret = err_rc_;                                                    \
            goto goto_tag;                                                    \
        }                                                                     \
    } while (0)

#define ESP_RETURN_ON_FALSE(a, err_code, log_tag, format, ...)                \
    do {                                                                      \
        if (!(a)) {                                                           \
            ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__,      \
                     ##__VA_ARGS__);                                          \
            return err_code;                                                  \
        }                                                                     \
    } while (0)

#define ESP_GOTO_ON_FALSE(a, err_code, goto_tag, log_tag, format, ...)        \
    do {                                                                      \
        if (!(a)) {                                                           \
            ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__,      \
                     ##__VA_ARGS__);                                          \
            ret = err_code;                                                   \
            goto goto_tag;                                                    \
        }                                                                     \
    } while (0)

#ifdef __cplusplus
}
#endif
