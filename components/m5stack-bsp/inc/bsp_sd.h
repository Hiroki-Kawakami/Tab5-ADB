/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * SD card mount/unmount. Mounting is the only board concern here: once
 * mounted, app code uses plain POSIX file I/O (open/read/opendir/stat) under
 * `mount_point` on both targets — the device through the ESP-IDF FATFS VFS,
 * the simulator through a path redirect onto a host directory (see
 * simulator/sd_redirect.c). There is no hot-plug detection: mount on demand
 * and treat a failure as "no card".
 *
 * Device read-performance note: prefer unbuffered read() in 16 KB chunks into
 * a heap_caps_malloc(..., MALLOC_CAP_CACHE_ALIGNED) buffer over fread() — the
 * FATFS VFS fast path needs a cache-aligned destination, and stdio's small
 * buffer serializes the transfer. CONFIG_FATFS_VFS_FSTAT_BLKSIZE is raised to
 * 4096 for the same reason (code that does go through stdio gets sane
 * buffering).
 */

#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool    format_if_mount_failed;  /*!< default: false */
    uint8_t max_files;               /*!< open-file limit; 0 -> 5 */
    int     max_freq_khz;            /*!< 0 -> SDMMC_FREQ_HIGHSPEED (40 MHz) */
} bsp_sd_mount_config_t;

/* Mount the SD card under `mount_point` (e.g. "/sd"). NULL config = defaults.
 * Returns ESP_ERR_INVALID_STATE when already mounted (callers mounting on
 * demand treat that as success). */
esp_err_t bsp_sd_mount(const char *mount_point, const bsp_sd_mount_config_t *config);

esp_err_t bsp_sd_unmount(void);

bool bsp_sd_is_mounted(void);

#ifdef __cplusplus
}
#endif
