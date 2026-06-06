/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "bsp_private.h"
#include "driver/gpio.h"
#include "driver/i2s_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct es8388_state *es8388_t;
typedef struct i2c_master_bus_t *i2c_master_bus_handle_t;

typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    uint8_t i2c_address;       /*!< 0 -> use ES8388_CODEC_DEFAULT_ADDR (0x20) */
    i2s_port_t i2s_port;
    gpio_num_t mclk_gpio;
    gpio_num_t bclk_gpio;
    gpio_num_t ws_gpio;
    gpio_num_t dout_gpio;
    gpio_num_t din_gpio;       /*!< Set to GPIO_NUM_NC to disable RX */
    uint32_t sample_rate;      /*!< 0 -> 48000 */
    uint8_t bits_per_sample;   /*!< 0 -> 16 */
    uint8_t channels;          /*!< 0 -> 2 */
} es8388_config_t;

esp_err_t es8388_init(const es8388_config_t *config, es8388_t *es8388);
esp_err_t es8388_deinit(es8388_t es8388);

esp_err_t es8388_open(es8388_t es8388, uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
esp_err_t es8388_close(es8388_t es8388);
esp_err_t es8388_reconfig_output(es8388_t es8388, uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
esp_err_t es8388_write(es8388_t es8388, const void *data, size_t len);
esp_err_t es8388_set_volume(es8388_t es8388, int volume);  /*!< 0..100, 0 mutes */
esp_err_t es8388_set_mute(es8388_t es8388, bool mute);
int       es8388_get_volume(es8388_t es8388);

#ifdef __cplusplus
}
#endif
