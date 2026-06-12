/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Tab5 audio provider: ES8388 codec (speaker/HP output over I2S0) plus the
 * PI4IOE1 expander's SPK_EN amp gate and HP_DET jack detect, wired into a
 * bsp_audio_t (caps PCM | SPEAKER | HEADPHONE). Created by tab5.c and
 * registered with bsp_audio_set_active(), which owns the click-free bring-up.
 */

#pragma once
#include "bsp_audio.h"
#include "driver/i2c_master.h"
#include "pi4io/pi4io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    pi4io_t io_expander;        /*!< PI4IOE1 (SPK_EN pin 1, HP_DET pin 7) */
} tab5_audio_config_t;

/* The provider registers closed (DAC down, no signal until open) and the amp
 * gate is left untouched (the board initialises SPK_EN low). */
esp_err_t tab5_audio_create(const tab5_audio_config_t *config, bsp_audio_t **out_audio);

#ifdef __cplusplus
}
#endif
