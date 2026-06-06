/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "bsp_private.h"
#include "bsp_tab5.h"
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "misc/bsp_display.h"
#include "pi4io/pi4io.h"
#include "ili9881c/ili9881c.h"
#include "gt911/gt911.h"
#include "st7123/st7123_lcd.h"
#include "st7123/st7123_touch.h"
#include "es8388/es8388.h"
#include "audio_eq.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_hosted.h"
#ifdef CONFIG_BT_BLUEDROID_ENABLED
#include "esp_hosted_bt.h"
#endif
#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "nimble/nimble_port.h"
#endif

static const char *TAG = "BSP_TAB5";

#define I2C0_PORT_NUM (0)
static i2c_master_bus_handle_t i2c0;
static pi4io_t pi4ioe1, pi4ioe2;

static void **frame_buffers;
static ili9881c_lcd_t ili9881c;
static gt911_touch_t gt911;
static st7123_lcd_t st7123_lcd;
static st7123_touch_t st7123_touch;
static es8388_t es8388;
static audio_eq_t audio_eq;
/* User-facing volume tracked here; hardware codec volume stays at max after
 * boot and audio_eq applies the user value as a software gain (with fade). */
static int s_user_volume = -1;
#define BSP_VOLUME_FADE_MS 100
#define BSP_VOLUME_DB_SPAN 40.0f   /* vol=1 → -40 dB, vol=100 → 0 dB */

#define SPK_EN_PIN  1
#define HP_DET_PIN  7
static volatile bsp_speaker_mode_t s_speaker_mode = BSP_SPEAKER_MODE_ON;
static TaskHandle_t s_speaker_task = NULL;
static portMUX_TYPE s_hp_mux = portMUX_INITIALIZER_UNLOCKED;
static bsp_headphone_cb_t s_hp_cb = NULL;
static void *s_hp_cb_arg = NULL;
static bool s_hp_last = false;
static bool s_hp_last_valid = false;

static bool hp_inserted_now(void) {
    if (!pi4ioe1) return false;
    bool hp = false;
    /* HP_DET is pulled up externally; the jack's NC detect switch shorts the
     * line to GND when no plug is inserted, so HP_DET=1 => headphones in. */
    if (pi4io_get_input(pi4ioe1, HP_DET_PIN, &hp) != ESP_OK) return false;
    return hp;
}

static void apply_speaker_pin_with_hp(bsp_speaker_mode_t mode, bool hp) {
    if (!pi4ioe1) return;
    bool desired;
    switch (mode) {
        case BSP_SPEAKER_MODE_ON:   desired = true; break;
        case BSP_SPEAKER_MODE_AUTO: desired = !hp;  break;
        case BSP_SPEAKER_MODE_OFF:
        default:                    desired = false; break;
    }
    pi4io_set_output(pi4ioe1, SPK_EN_PIN, desired);
}

static void apply_speaker_pin(bsp_speaker_mode_t mode) {
    apply_speaker_pin_with_hp(mode, hp_inserted_now());
}

static void speaker_task(void *arg) {
    (void)arg;
    while (1) {
        bsp_speaker_mode_t mode = s_speaker_mode;
        bool hp = hp_inserted_now();

        /* Detect HP state change and dispatch callback (fired outside the
         * critical section so user code can take its time / call into BSP). */
        bsp_headphone_cb_t cb = NULL;
        void *cb_arg = NULL;
        bool fire = false;
        portENTER_CRITICAL(&s_hp_mux);
        if (s_hp_last_valid && hp != s_hp_last && s_hp_cb) {
            cb = s_hp_cb;
            cb_arg = s_hp_cb_arg;
            fire = true;
        }
        s_hp_last = hp;
        s_hp_last_valid = true;
        portEXIT_CRITICAL(&s_hp_mux);
        if (fire) cb(hp, cb_arg);

        apply_speaker_pin_with_hp(mode, hp);

        bool need_poll = (mode == BSP_SPEAKER_MODE_AUTO) || (s_hp_cb != NULL);
        TickType_t wait = need_poll ? pdMS_TO_TICKS(200) : portMAX_DELAY;
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

static esp_err_t start_speaker_task_once(void) {
    if (s_speaker_task) return ESP_OK;
    return xTaskCreate(speaker_task, "bsp_spk", 2048, NULL, 1, &s_speaker_task) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t bsp_tab5_init(const bsp_tab5_config_t *config) {
    esp_err_t err;

    // Check config values
    bsp_tab5_config_t tmp_config = *config;
    if (!tmp_config.display.fb_num) tmp_config.display.fb_num = 1;
    config = &tmp_config;

    // Initialize I2C0 bus
    err = i2c_new_master_bus(&(i2c_master_bus_config_t){
        .i2c_port = I2C0_PORT_NUM,
        .sda_io_num = GPIO_NUM_31,
        .scl_io_num = GPIO_NUM_32,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    }, &i2c0);
    BSP_RETURN_ERR(err);

    // Initialize PI4IOE1 (address 0x43)
    err = pi4io_init(i2c0, 0x43, (pi4io_pin_config_t[8]){
        [0] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = false },  // RF_INT_EXT_SWITCH
        [1] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = false },  // SPK_EN — set by speaker_mode below
        [2] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = true },   // EXT5V_EN
        [4] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = true },   // LCD_RST
        [5] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = true },   // TP_RST
        [6] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = true },   // CAM_RST
        [7] = { PI4IO_PIN_MODE_INPUT },                           // HP_DET
    }, &pi4ioe1);
    BSP_RETURN_ERR(err);

    // Speaker amp policy is captured here but the SPK_EN line stays LOW for
    // now — we don't enable the amp until *after* the codec is initialised,
    // unmuted, and feeding stable silence. Enabling the amp earlier causes:
    //   (1) the amp's own power-on transient, and
    //   (2) any DC offset / unmute click on the codec's analog output to be
    //       amplified into the speaker.
    // Both surface as the audible "ブツッ" at boot. Delaying SPK_EN until the
    // DAC is settled removes (2) entirely and quietens (1).
    s_speaker_mode = config->audio.speaker_mode;

    // Initialize PI4IOE2 (address 0x44)
    err = pi4io_init(i2c0, 0x44, (pi4io_pin_config_t[8]){
        [0] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = true },   // WLAN_PWR_EN
        [3] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = config->usb.usb5v_en }, // USB5V_EN
        [4] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = false },  // PWROFF_PLUSE
        [5] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = false },  // nCHG_QC_EN
        [6] = { PI4IO_PIN_MODE_INPUT },                           // CHG_STAT
        [7] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = false },  // CHG_EN
    }, &pi4ioe2);
    BSP_RETURN_ERR(err);

    // Reset Touch Panel and LCD
    gpio_reset_pin(GPIO_NUM_23);
    pi4io_set_output(pi4ioe1, 4, false);  // LCD_RST = Low
    pi4io_set_output(pi4ioe1, 5, false);  // TP_RST = Low
    vTaskDelay(pdMS_TO_TICKS(100));
    pi4io_set_output(pi4ioe1, 4, true);   // LCD_RST = High
    pi4io_set_output(pi4ioe1, 5, true);   // TP_RST = High
    vTaskDelay(pdMS_TO_TICKS(100));

    if (i2c_master_probe(i2c0, 0x55, 10) == ESP_OK) {
        // Initialize ST7123 LCD
        err = st7123_lcd_init(&(st7123_lcd_config_t){
            .backlight_gpio = GPIO_NUM_22,
            .size = (bsp_size_t){ 720, 1280 },
            .pixel_format = config->display.pixel_format,
            .fb_num = config->display.fb_num,
        }, &st7123_lcd);
        BSP_RETURN_ERR(err);
        frame_buffers = st7123_lcd_get_frame_buffers(st7123_lcd);

        // Initialize ST7123 Touch Panel
        err = st7123_touch_init(&(st7123_touch_config_t){
            .i2c_bus = i2c0,
            .size = (bsp_size_t){ 720, 1280 },
            .int_gpio = GPIO_NUM_23,
            .rst_gpio = GPIO_NUM_NC,
            .scl_speed_hz = 100000,
            .interrupt = config->touch.interrupt,
        }, &st7123_touch);
        BSP_RETURN_ERR(err);
    } else if (i2c_master_probe(i2c0, 0x14, 10) == ESP_OK) {
        // Initialize ILI9881C LCD
        err = ili9881c_lcd_init(&(ili9881c_lcd_config_t){
            .backlight_gpio = GPIO_NUM_22,
            .size = (bsp_size_t){ 720, 1280 },
            .pixel_format = config->display.pixel_format,
            .fb_num = config->display.fb_num,
        }, &ili9881c);
        BSP_RETURN_ERR(err);
        frame_buffers = ili9881c_lcd_get_frame_buffers(ili9881c);

        // Initialize GT911 Touch Panel
        err = gt911_touch_init(&(gt911_touch_config_t){
            .i2c_bus = i2c0,
            .size = (bsp_size_t){ 720, 1280 },
            .int_gpio = GPIO_NUM_23,
            .rst_gpio = GPIO_NUM_NC,
            .scl_speed_hz = 100000,
            .interrupt = config->touch.interrupt,
        }, &gt911);
        BSP_RETURN_ERR(err);
    } else {
        return ESP_ERR_NOT_FOUND;
    }

    // ES8388 audio codec (speaker output)
    if (!config->audio.disable) {
        err = es8388_init(&(es8388_config_t){
            .i2c_bus         = i2c0,
            .i2s_port        = I2S_NUM_0,
            .mclk_gpio       = GPIO_NUM_30,
            .bclk_gpio       = GPIO_NUM_27,
            .ws_gpio         = GPIO_NUM_29,
            .dout_gpio       = GPIO_NUM_26,
            .din_gpio        = GPIO_NUM_28,
            .sample_rate     = config->audio.sample_rate,
            .bits_per_sample = config->audio.bits_per_sample,
            .channels        = config->audio.channels,
        }, &es8388);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ES8388 init failed: %d (continuing without audio)", err);
            es8388 = NULL;
        } else {
            uint32_t rate = config->audio.sample_rate    ? config->audio.sample_rate    : 48000;
            uint8_t  bps  = config->audio.bits_per_sample ? config->audio.bits_per_sample : 16;
            uint8_t  ch   = config->audio.channels        ? config->audio.channels        : 2;
            audio_eq_config_t eq_cfg = {
                .sample_rate        = rate,
                .channels           = ch,
                .bits_per_sample    = bps,
                .max_stages         = config->audio.eq.max_stages,
                .enabled            = config->audio.eq.enable,
                .initial_biquads    = config->audio.eq.biquads,
                .initial_num_stages = config->audio.eq.num_stages,
            };
            err = audio_eq_init(&eq_cfg, &audio_eq);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "audio_eq_init failed: %d (EQ disabled)", err);
                audio_eq = NULL;
            } else {
                /* Start silent so the codec's unmute / volume jump below is
                 * masked — there is no signal to click on while gain=0. */
                audio_eq_set_gain(audio_eq, 0.0f, 0);
                /* Pin codec to max once; user volume is delivered by the
                 * software gain (with fade) from now on. SPK_EN is still LOW
                 * at this point so the unmute click doesn't reach the speaker. */
                es8388_set_volume(es8388, 100);
            }
        }
    }

    /* Now that the DAC output is steady silence, give the analog stage a
     * moment to settle, then enable the speaker amp. The amp's own startup
     * transient is unavoidable but is now the only remaining click source. */
    if (es8388) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    apply_speaker_pin(s_speaker_mode);
    if (s_speaker_mode == BSP_SPEAKER_MODE_AUTO || s_hp_cb) {
        err = start_speaker_task_once();
        BSP_RETURN_ERR(err);
    }

    if (config->wifi.mode || config->bluetooth.enable) {
        // NVS (for WiFi & Bluetooth)
        err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            if ((err = nvs_flash_erase()) == ESP_OK) {
                err = nvs_flash_init();
            }
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize NVS flash");
            return err;
        }
    }

    // WiFi
    if (config->wifi.mode) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        if (config->wifi.mode & BSP_WIFI_MODE_STA) esp_netif_create_default_wifi_sta();
        if (config->wifi.mode & BSP_WIFI_MODE_AP) esp_netif_create_default_wifi_ap();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    }

    // Bluetooth
    if (config->bluetooth.enable) {
#if defined(CONFIG_BT_BLUEDROID_ENABLED)
        /* initialize TRANSPORT first */
        hosted_hci_bluedroid_open();

        /* get HCI driver operations */
        esp_bluedroid_hci_driver_operations_t operations = {
            .send = hosted_hci_bluedroid_send,
            .check_send_available = hosted_hci_bluedroid_check_send_available,
            .register_host_callback = hosted_hci_bluedroid_register_host_callback,
        };
        esp_bluedroid_attach_hci_driver(&operations);
#elif defined(CONFIG_BT_NIMBLE_ENABLED)
        err = nimble_port_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize NimBLE");
            return err;
        }
#else
        ESP_LOGE(TAG, "Bluetooth Stack is not Enabled.");
#endif
    }

    return ESP_OK;
}

void bsp_tab5_restart(void) {
    /* Codec runs pinned at vol=100 (SW gain delivers the user volume), so
     * cutting the analog output through the codec's own mute register is the
     * direct way to silence the DAC before the I2S clocks die. */
    if (es8388) es8388_set_mute(es8388, true);
    /* Drop SPK_EN directly rather than going through set_speaker_mode — the
     * speaker_task may not get scheduled before esp_restart cuts everything,
     * and the pi4io I2C write is what we actually need to land. */
    if (pi4ioe1) pi4io_set_output(pi4ioe1, SPK_EN_PIN, false);
    /* Black out the panel so the brief reset window doesn't flash whatever
     * happens to be in the framebuffer. */
    bsp_tab5_display_set_brightness(0);
    /* Let the I2C writes complete and the DAC analog stage settle. */
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}

// MARK: Display
void bsp_tab5_display_set_brightness(int brightness) {
    if (ili9881c) ili9881c_lcd_set_brightness(ili9881c, brightness);
    if (st7123_lcd) st7123_lcd_set_brightness(st7123_lcd, brightness);
}
void *bsp_tab5_display_get_frame_buffer(int fb_index) {
    return frame_buffers[fb_index];
}
void bsp_tab5_display_flush(int fb_index) {
    if (ili9881c) ili9881c_lcd_flush(ili9881c, fb_index);
    if (st7123_lcd) st7123_lcd_flush(st7123_lcd, fb_index);
}

// MARK: Touch Panel
int bsp_tab5_touch_read(esp_lcd_touch_point_data_t *points, uint8_t max_points) {
    if (gt911) return gt911_touch_read(gt911, points, max_points);
    if (st7123_touch) return st7123_touch_read(st7123_touch, points, max_points);
    return 0;
}
void bsp_tab5_touch_wait_interrupt(void) {
    if (gt911) gt911_touch_wait_interrupt(gt911);
    if (st7123_touch) st7123_touch_wait_interrupt(st7123_touch);
}

// MARK: Audio
esp_err_t bsp_tab5_audio_open(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels) {
    if (!es8388) return ESP_ERR_INVALID_STATE;
    return es8388_open(es8388, sample_rate, bits_per_sample, channels);
}
esp_err_t bsp_tab5_audio_close(void) {
    if (!es8388) return ESP_ERR_INVALID_STATE;
    return es8388_close(es8388);
}
esp_err_t bsp_tab5_audio_reconfig(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels) {
    if (!es8388) return ESP_ERR_INVALID_STATE;
    esp_err_t err = es8388_reconfig_output(es8388, sample_rate, bits_per_sample, channels);
    if (err == ESP_OK && audio_eq) {
        audio_eq_reconfig(audio_eq, sample_rate, channels, bits_per_sample);
    }
    return err;
}
esp_err_t bsp_tab5_audio_write(void *data, size_t len) {
    if (!es8388) return ESP_ERR_INVALID_STATE;
    /* Always go through audio_eq even when biquads are disabled — the gain
     * fader lives here too and skipping it would bypass the volume control. */
    if (audio_eq) audio_eq_process(audio_eq, data, len);
    return es8388_write(es8388, data, len);
}
esp_err_t bsp_tab5_audio_set_volume(int volume) {
    if (!es8388) return ESP_ERR_INVALID_STATE;
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;
    if (volume == s_user_volume) return ESP_OK;  /* drop slider duplicates */
    s_user_volume = volume;
    if (audio_eq) {
        /* Linear-in-dB curve: vol=100 → 0 dB (gain 1.0), vol=1 → -40 dB.
         * vol=0 is a hard zero so muting via volume=0 is true silence. */
        float gain;
        if (volume == 0) {
            gain = 0.0f;
        } else {
            float db = (volume - 100) * (BSP_VOLUME_DB_SPAN / 100.0f);
            gain = powf(10.0f, db / 20.0f);
        }
        return audio_eq_set_gain(audio_eq, gain, BSP_VOLUME_FADE_MS);
    }
    /* No EQ instance → fall back to direct hardware volume (clicky). */
    return es8388_set_volume(es8388, volume);
}
esp_err_t bsp_tab5_audio_set_mute(bool mute) {
    if (!es8388) return ESP_ERR_INVALID_STATE;
    return es8388_set_mute(es8388, mute);
}
int bsp_tab5_audio_get_volume(void) {
    if (s_user_volume >= 0) return s_user_volume;
    return es8388 ? es8388_get_volume(es8388) : -1;
}

esp_err_t bsp_tab5_audio_eq_set_enabled(bool enabled) {
    if (!audio_eq) return ESP_ERR_INVALID_STATE;
    return audio_eq_set_enabled(audio_eq, enabled);
}
bool bsp_tab5_audio_eq_is_enabled(void) {
    return audio_eq_is_enabled(audio_eq);
}
esp_err_t bsp_tab5_audio_eq_set_biquads(const audio_eq_biquad_t *biquads, size_t num_stages) {
    if (!audio_eq) return ESP_ERR_INVALID_STATE;
    return audio_eq_set_biquads(audio_eq, biquads, num_stages);
}
audio_eq_t bsp_tab5_audio_eq_handle(void) {
    return audio_eq;
}

esp_err_t bsp_tab5_audio_set_speaker_mode(bsp_speaker_mode_t mode) {
    if (mode != BSP_SPEAKER_MODE_ON && mode != BSP_SPEAKER_MODE_AUTO &&
        mode != BSP_SPEAKER_MODE_OFF) return ESP_ERR_INVALID_ARG;
    if (!pi4ioe1) return ESP_ERR_INVALID_STATE;
    s_speaker_mode = mode;
    if (mode == BSP_SPEAKER_MODE_AUTO) {
        esp_err_t err = start_speaker_task_once();
        if (err != ESP_OK) return err;
    }
    if (s_speaker_task) {
        xTaskNotifyGive(s_speaker_task);  /* task re-evaluates + re-arms wait */
    } else {
        apply_speaker_pin(mode);
    }
    return ESP_OK;
}
bsp_speaker_mode_t bsp_tab5_audio_get_speaker_mode(void) {
    return s_speaker_mode;
}
bool bsp_tab5_audio_headphone_inserted(void) {
    return hp_inserted_now();
}

esp_err_t bsp_tab5_audio_set_mono_mix(bool enabled) {
    if (!audio_eq) return ESP_ERR_INVALID_STATE;
    return audio_eq_set_mono_mix(audio_eq, enabled);
}
bool bsp_tab5_audio_get_mono_mix(void) {
    return audio_eq_get_mono_mix(audio_eq);
}

esp_err_t bsp_tab5_audio_set_headphone_callback(bsp_headphone_cb_t cb, void *user) {
    if (!pi4ioe1) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_hp_mux);
    s_hp_cb_arg = user;
    s_hp_cb     = cb;
    portEXIT_CRITICAL(&s_hp_mux);
    if (cb) {
        esp_err_t err = start_speaker_task_once();
        if (err != ESP_OK) return err;
        xTaskNotifyGive(s_speaker_task);  /* re-evaluate need_poll */
    }
    return ESP_OK;
}
