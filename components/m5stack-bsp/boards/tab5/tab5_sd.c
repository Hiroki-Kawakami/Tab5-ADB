/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Tab5 SD card: the TF slot is on the P4's SDMMC peripheral (slot 0; slot 1 is
 * the C6 Wi-Fi SDIO link), routed like the ESP32-P4 EV board — CLK=G43, CMD=G44,
 * D0..D3=G39..G42 (4-bit), card power from the P4's on-chip LDO channel 4 (VO4).
 * Mounts a FAT filesystem through the ESP-IDF VFS; see bsp_sd.h for the
 * read-performance guidance.
 */

#include "bsp_sd.h"

#include <string.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

static const char *TAG = "BSP_SD";

#define SD_PIN_CLK     GPIO_NUM_43
#define SD_PIN_CMD     GPIO_NUM_44
#define SD_PIN_D0      GPIO_NUM_39
#define SD_PIN_D1      GPIO_NUM_40
#define SD_PIN_D2      GPIO_NUM_41
#define SD_PIN_D3      GPIO_NUM_42
#define SD_LDO_CHAN_ID 4

static sdmmc_card_t *s_card;
static char s_mount_point[32];
/* The LDO powers the slot; keep it acquired across unmount/remount cycles. */
static sd_pwr_ctrl_handle_t s_pwr_ctrl;

esp_err_t bsp_sd_mount(const char *mount_point, const bsp_sd_mount_config_t *config) {
    static const bsp_sd_mount_config_t defaults = {0};
    if (!config) config = &defaults;
    if (!mount_point || strlen(mount_point) >= sizeof(s_mount_point)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_card) return ESP_ERR_INVALID_STATE;

    if (!s_pwr_ctrl) {
        sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = SD_LDO_CHAN_ID };
        esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_pwr_ctrl);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "sd_pwr_ctrl_new_on_chip_ldo: %s", esp_err_to_name(err));
            return err;
        }
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    /* SD is on SDMMC slot 0; slot 1 is reserved for the C6 Wi-Fi SDIO link
     * (esp_hosted). SDMMC_HOST_DEFAULT() picks slot 1, so override it — both on
     * slot 1 collide on the shared host and reset the C6 link mid-transfer. */
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = config->max_freq_khz > 0 ? config->max_freq_khz
                                                 : SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = s_pwr_ctrl;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = SD_PIN_CLK;
    slot_config.cmd = SD_PIN_CMD;
    slot_config.d0 = SD_PIN_D0;
    slot_config.d1 = SD_PIN_D1;
    slot_config.d2 = SD_PIN_D2;
    slot_config.d3 = SD_PIN_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = config->format_if_mount_failed,
        .max_files              = config->max_files ? config->max_files : 5,
        .allocation_unit_size   = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config,
                                            &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount %s failed: %s", mount_point, esp_err_to_name(err));
        s_card = NULL;
        return err;
    }

    strcpy(s_mount_point, mount_point);
    ESP_LOGI(TAG, "mounted %s: %s, %llu MB", mount_point, s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
    return ESP_OK;
}

esp_err_t bsp_sd_unmount(void) {
    if (!s_card) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_vfs_fat_sdcard_unmount(s_mount_point, s_card);
    s_card = NULL;
    return err;
}

bool bsp_sd_is_mounted(void) {
    return s_card != NULL;
}
