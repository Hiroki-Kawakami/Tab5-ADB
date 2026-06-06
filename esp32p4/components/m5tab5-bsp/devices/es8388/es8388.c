/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "es8388.h"
#include <string.h>
#include <stdlib.h>
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8388_codec.h"
#include "audio_codec_ctrl_if.h"
#include "audio_codec_data_if.h"
#include "audio_codec_if.h"

static const char *TAG = "ES8388";

struct es8388_state {
    i2s_chan_handle_t tx_handle;
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_data_if_t *data_if;
    const audio_codec_if_t *codec_if;
    esp_codec_dev_handle_t output_device;
    int volume;
};

static i2s_data_bit_width_t to_bit_width(uint8_t bps) {
    switch (bps) {
        case 8:  return I2S_DATA_BIT_WIDTH_8BIT;
        case 16: return I2S_DATA_BIT_WIDTH_16BIT;
        case 24: return I2S_DATA_BIT_WIDTH_24BIT;
        case 32: return I2S_DATA_BIT_WIDTH_32BIT;
        default: return I2S_DATA_BIT_WIDTH_16BIT;
    }
}

esp_err_t es8388_init(const es8388_config_t *config, es8388_t *es8388) {
    if (!config || !es8388 || !config->i2c_bus) return ESP_ERR_INVALID_ARG;

    struct es8388_state *state = calloc(1, sizeof(*state));
    if (!state) return ESP_ERR_NO_MEM;
    state->volume = -1;

    uint32_t rate = config->sample_rate ? config->sample_rate : 48000;
    uint8_t  bps  = config->bits_per_sample ? config->bits_per_sample : 16;
    uint8_t  ch   = config->channels ? config->channels : 2;

    esp_err_t ret;

    // I2S channel (TX only — speaker output)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(config->i2s_port, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;
    ret = i2s_new_channel(&chan_cfg, &state->tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %d", ret);
        goto err_free;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(to_bit_width(bps), I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = config->mclk_gpio,
            .bclk = config->bclk_gpio,
            .ws   = config->ws_gpio,
            .dout = config->dout_gpio,
            .din  = config->din_gpio,
            .invert_flags = { 0 },
        },
    };
    ret = i2s_channel_init_std_mode(state->tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %d", ret);
        goto err_chan;
    }
    ret = i2s_channel_enable(state->tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %d", ret);
        goto err_chan;
    }

    // I2C control interface to the ES8388
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 0,
        .addr = config->i2c_address ? config->i2c_address : ES8388_CODEC_DEFAULT_ADDR,
        .bus_handle = config->i2c_bus,
    };
    state->ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!state->ctrl_if) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
        ret = ESP_FAIL;
        goto err_chan;
    }

    // ES8388 codec
    es8388_codec_cfg_t es_cfg = {
        .ctrl_if     = state->ctrl_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .master_mode = false,
        .pa_pin      = -1,
    };
    state->codec_if = es8388_codec_new(&es_cfg);
    if (!state->codec_if) {
        ESP_LOGE(TAG, "es8388_codec_new failed");
        ret = ESP_FAIL;
        goto err_ctrl;
    }

    // I2S data interface
    audio_codec_i2s_cfg_t data_cfg = {
        .port      = config->i2s_port,
        .tx_handle = state->tx_handle,
    };
    state->data_if = audio_codec_new_i2s_data(&data_cfg);
    if (!state->data_if) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
        ret = ESP_FAIL;
        goto err_codec;
    }

    // Codec device (output)
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = state->codec_if,
        .data_if  = state->data_if,
    };
    state->output_device = esp_codec_dev_new(&dev_cfg);
    if (!state->output_device) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        ret = ESP_FAIL;
        goto err_data;
    }

    esp_codec_dev_sample_info_t info = {
        .sample_rate     = rate,
        .channel         = ch,
        .bits_per_sample = bps,
    };
    ret = esp_codec_dev_open(state->output_device, &info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", ret);
        goto err_dev;
    }

    // Start muted — caller picks a volume.
    esp_codec_dev_set_out_mute(state->output_device, true);
    state->volume = 0;

    *es8388 = state;
    return ESP_OK;

err_dev:
    esp_codec_dev_delete(state->output_device);
err_data:
    audio_codec_delete_data_if(state->data_if);
err_codec:
    audio_codec_delete_codec_if(state->codec_if);
err_ctrl:
    audio_codec_delete_ctrl_if(state->ctrl_if);
err_chan:
    if (state->tx_handle) {
        i2s_channel_disable(state->tx_handle);
        i2s_del_channel(state->tx_handle);
    }
err_free:
    free(state);
    return ret;
}

esp_err_t es8388_deinit(es8388_t es8388) {
    if (!es8388) return ESP_ERR_INVALID_ARG;
    if (es8388->output_device) {
        esp_codec_dev_close(es8388->output_device);
        esp_codec_dev_delete(es8388->output_device);
    }
    if (es8388->data_if)  audio_codec_delete_data_if(es8388->data_if);
    if (es8388->codec_if) audio_codec_delete_codec_if(es8388->codec_if);
    if (es8388->ctrl_if)  audio_codec_delete_ctrl_if(es8388->ctrl_if);
    if (es8388->tx_handle) {
        i2s_channel_disable(es8388->tx_handle);
        i2s_del_channel(es8388->tx_handle);
    }
    free(es8388);
    return ESP_OK;
}

esp_err_t es8388_open(es8388_t es8388, uint32_t rate, uint8_t bps, uint8_t ch) {
    if (!es8388) return ESP_ERR_INVALID_ARG;
    esp_codec_dev_sample_info_t info = {
        .sample_rate     = rate,
        .channel         = ch,
        .bits_per_sample = bps,
    };
    return esp_codec_dev_open(es8388->output_device, &info);
}

esp_err_t es8388_close(es8388_t es8388) {
    if (!es8388) return ESP_ERR_INVALID_ARG;
    return esp_codec_dev_close(es8388->output_device);
}

esp_err_t es8388_reconfig_output(es8388_t es8388, uint32_t rate, uint8_t bps, uint8_t ch) {
    if (!es8388) return ESP_ERR_INVALID_ARG;
    esp_codec_dev_close(es8388->output_device);
    esp_codec_dev_sample_info_t info = {
        .sample_rate     = rate,
        .channel         = ch,
        .bits_per_sample = bps,
    };
    return esp_codec_dev_open(es8388->output_device, &info);
}

esp_err_t es8388_write(es8388_t es8388, const void *data, size_t len) {
    if (!es8388 || !data) return ESP_ERR_INVALID_ARG;
    return esp_codec_dev_write(es8388->output_device, (void *)data, (int)len);
}

esp_err_t es8388_set_volume(es8388_t es8388, int volume) {
    if (!es8388) return ESP_ERR_INVALID_ARG;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    es8388->volume = volume;
    if (volume == 0) {
        return esp_codec_dev_set_out_mute(es8388->output_device, true);
    }
    esp_err_t ret = esp_codec_dev_set_out_mute(es8388->output_device, false);
    if (ret != ESP_OK) return ret;
    return esp_codec_dev_set_out_vol(es8388->output_device, volume);
}

esp_err_t es8388_set_mute(es8388_t es8388, bool mute) {
    if (!es8388) return ESP_ERR_INVALID_ARG;
    return esp_codec_dev_set_out_mute(es8388->output_device, mute);
}

int es8388_get_volume(es8388_t es8388) {
    return es8388 ? es8388->volume : -1;
}
