#pragma once
// Host counterpart of ESP-IDF <esp_err.h>: the esp_err_t type, the common
// ESP_ERR_* codes, esp_err_to_name(), and the ESP_ERROR_CHECK family.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK                    0
#define ESP_FAIL                 -1
#define ESP_ERR_NO_MEM            0x101
#define ESP_ERR_INVALID_ARG       0x102
#define ESP_ERR_INVALID_STATE     0x103
#define ESP_ERR_INVALID_SIZE      0x104
#define ESP_ERR_NOT_FOUND         0x105
#define ESP_ERR_NOT_SUPPORTED     0x106
#define ESP_ERR_TIMEOUT           0x107
#define ESP_ERR_INVALID_RESPONSE  0x108
#define ESP_ERR_INVALID_CRC       0x109
#define ESP_ERR_INVALID_VERSION   0x10A
#define ESP_ERR_INVALID_MAC       0x10B
#define ESP_ERR_NOT_FINISHED      0x10C
#define ESP_ERR_NOT_ALLOWED       0x10D

const char *esp_err_to_name(esp_err_t code);

/* Abort if the expression is not ESP_OK (mirrors ESP-IDF's macro, which panics
 * on device; here we print and abort()). */
#define ESP_ERROR_CHECK(x)                                                     \
    do {                                                                       \
        esp_err_t err_rc_ = (x);                                               \
        if (err_rc_ != ESP_OK) {                                              \
            fprintf(stderr, "ESP_ERROR_CHECK failed: %s (0x%x) at %s:%d\n",   \
                    esp_err_to_name(err_rc_), err_rc_, __FILE__, __LINE__);    \
            abort();                                                           \
        }                                                                      \
    } while (0)

/* Like ESP_ERROR_CHECK but returns the code instead of aborting. */
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x)                                       \
    ({                                                                         \
        esp_err_t err_rc_ = (x);                                               \
        if (err_rc_ != ESP_OK) {                                              \
            fprintf(stderr, "ESP_ERROR_CHECK_WITHOUT_ABORT failed: %s (0x%x) " \
                    "at %s:%d\n", esp_err_to_name(err_rc_), err_rc_,           \
                    __FILE__, __LINE__);                                       \
        }                                                                      \
        err_rc_;                                                               \
    })

#ifdef __cplusplus
}
#endif
