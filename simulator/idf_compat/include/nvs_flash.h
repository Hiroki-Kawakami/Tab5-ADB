#pragma once
// Host counterpart of ESP-IDF <nvs_flash.h>. Like the real header it pulls in
// <nvs.h>, so including this is enough to use the whole NVS API.
#include "esp_err.h"
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);

/* Simulator-only extension: choose the JSON file that backs the store. Call
 * before nvs_flash_init() / the first nvs_open(). Defaults to "nvs_data.json"
 * in the working directory. Has no on-device counterpart. */
void nvs_flash_sim_set_path(const char *path);

#ifdef __cplusplus
}
#endif
