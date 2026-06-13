/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * M5Stack Tab5 board: brings up the bus + IO expanders, resolves the panel
 * generation (ST7123 vs ILI9881C/GT911) by I2C probe, and wires the resulting
 * bsp_display / bsp_touch / bsp_audio providers. Everything is accessed
 * through the provider vtables; policy (speaker route, volume curve, the
 * click-free audio sequencing) lives in the shared dispatch layers.
 */

#include "bsp_private.h"
#include "bsp.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "bsp_touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "pi4io/pi4io.h"
#include "ili9881c/ili9881c.h"
#include "gt911/gt911.h"
#include "st7123/st7123_lcd.h"
#include "st7123/st7123_touch.h"
#include "tab5_audio.h"
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

esp_err_t bsp_init(const bsp_config_t *config) {
    esp_err_t err;

    // Check config values
    bsp_config_t tmp_config = *config;
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

    // Initialize PI4IOE1 (address 0x43). SPK_EN starts LOW: the amp gate is
    // only enabled by bsp_audio_set_active() once the codec is initialised,
    // unmuted, and feeding stable silence (the click-free contract — enabling
    // it earlier amplifies the codec's power-on/unmute transient into the
    // audible "ブツッ" at boot).
    err = pi4io_init(i2c0, 0x43, (pi4io_pin_config_t[8]){
        [0] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = false },  // RF_INT_EXT_SWITCH
        [1] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = false },  // SPK_EN
        [2] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = true },   // EXT5V_EN
        [4] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = true },   // LCD_RST
        [5] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = true },   // TP_RST
        [6] = { PI4IO_PIN_MODE_OUTPUT, .initial_value = true },   // CAM_RST
        [7] = { PI4IO_PIN_MODE_INPUT },                           // HP_DET
    }, &pi4ioe1);
    BSP_RETURN_ERR(err);

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

    // Display + touch are panel-generation dependent. Probe the touch
    // controller address to pick the generation, then bring up the matching
    // bsp_display / bsp_touch providers.
    bsp_display_config_t display_config = {
        .backlight_gpio = GPIO_NUM_22,
        .size = (bsp_size_t){ 720, 1280 },
        .pixel_format = config->display.pixel_format,
        .fb_num = config->display.fb_num,
    };
    bsp_touch_config_t touch_config = {
        .i2c_bus = i2c0,
        .size = (bsp_size_t){ 720, 1280 },
        .int_gpio = GPIO_NUM_23,
        .rst_gpio = GPIO_NUM_NC,
        .scl_speed_hz = 100000,
        .interrupt = config->touch.interrupt,
    };
    bsp_display_t *display = NULL;
    bsp_touch_t *touch = NULL;
    if (i2c_master_probe(i2c0, 0x55, 10) == ESP_OK) {
        err = st7123_lcd_create(&display_config, &display);
        BSP_RETURN_ERR(err);
        err = st7123_touch_create(&touch_config, &touch);
        BSP_RETURN_ERR(err);
    } else if (i2c_master_probe(i2c0, 0x14, 10) == ESP_OK) {
        err = ili9881c_lcd_create(&display_config, &display);
        BSP_RETURN_ERR(err);
        err = gt911_touch_create(&touch_config, &touch);
        BSP_RETURN_ERR(err);
    } else {
        return ESP_ERR_NOT_FOUND;
    }
    bsp_display_set_active(display);
    bsp_touch_set_active(touch);

    // Audio: ES8388 codec + PI4IOE1 amp gate / HP detect as the bsp_audio
    // provider. Registers closed — no signal (and no amp) until the app's
    // first bsp_audio_open() runs the click-free bring-up.
    {
        bsp_audio_t *audio = NULL;
        err = tab5_audio_create(&(tab5_audio_config_t){
            .i2c_bus     = i2c0,
            .io_expander = pi4ioe1,
        }, &audio);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "audio init failed: %d (continuing without audio)", err);
        } else {
            bsp_audio_set_active(audio, &(bsp_audio_init_t){
                .dsp_mode     = config->audio.dsp_mode,
                .speaker_mode = config->audio.speaker_mode,
            });
        }
    }

    if (config->bluetooth.enable) {
        // NVS (for Bluetooth). Wi-Fi lifecycle (esp_netif/esp_wifi) is owned by the
        // `wifi` component, not the BSP — the C6 esp-hosted transport is brought up
        // by esp_wifi_init() itself, driven entirely by sdkconfig (SDIO pins / reset
        // GPIO / slave target), so the board needs no Wi-Fi bring-up code. NVS is
        // ensured idempotently by each consumer (settings, adb keystore, wifi).
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

void bsp_restart(void) {
    /* Silence the audio path (codec mute + amp gate off) before the I2S
     * clocks die. */
    bsp_audio_quiesce();
    /* Black out the panel so the brief reset window doesn't flash whatever
     * happens to be in the framebuffer. */
    bsp_display_set_brightness(0);
    /* Let the I2C writes complete and the DAC analog stage settle. */
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}

void bsp_usb_host_set_power(bool on) {
    /* USB5V_EN is PI4IOE2 (0x44) pin 3 — the host-port 5V load switch. */
    pi4io_set_output(pi4ioe2, 3, on);
}
