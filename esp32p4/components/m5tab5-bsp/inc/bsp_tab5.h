/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "bsp_common.h"
#include "audio_eq.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BSP_SPEAKER_MODE_ON       = 0,  /*!< amp always on (default — matches zero-init) */
    BSP_SPEAKER_MODE_AUTO     = 1,  /*!< amp on only while HP jack is unplugged */
    BSP_SPEAKER_MODE_OFF      = 2,  /*!< amp always off */
} bsp_speaker_mode_t;

typedef struct {
    struct {
        uint8_t fb_num;
        bsp_pixel_format_t pixel_format;
    } display;
    struct {
        bool interrupt;
    } touch;
    struct {
        bool usb5v_en;
    } usb;
    struct {
        bsp_wifi_mode_t mode;
    } wifi;
    struct {
        bool enable;
    } bluetooth;
    struct {
        bool disable;            /*!< Skip audio codec init (default: enabled) */
        uint32_t sample_rate;    /*!< 0 -> 48000 */
        uint8_t bits_per_sample; /*!< 0 -> 16 */
        uint8_t channels;        /*!< 0 -> 2 */
        struct {
            bool enable;                          /*!< Enable EQ at boot */
            size_t num_stages;                    /*!< Number of biquads in `biquads` */
            const audio_eq_biquad_t *biquads;     /*!< Initial coefficients (copied) */
            size_t max_stages;                    /*!< Capacity; 0 -> max(num_stages, 8) */
        } eq;
        bsp_speaker_mode_t speaker_mode;          /*!< Speaker amp policy at boot */
    } audio;
} bsp_tab5_config_t;

esp_err_t bsp_tab5_init(const bsp_tab5_config_t *config);
void bsp_tab5_restart(void);
void bsp_tab5_display_set_brightness(int brightness);
void *bsp_tab5_display_get_frame_buffer(int fb_index);
void bsp_tab5_display_flush(int fb_index);
int bsp_tab5_touch_read(esp_lcd_touch_point_data_t *points, uint8_t max_points);
void bsp_tab5_touch_wait_interrupt(void);

// MARK: Audio (speaker output via ES8388)
esp_err_t bsp_tab5_audio_open(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
esp_err_t bsp_tab5_audio_close(void);
esp_err_t bsp_tab5_audio_reconfig(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
/* When EQ is enabled, `data` is filtered in-place — caller must own the buffer. */
esp_err_t bsp_tab5_audio_write(void *data, size_t len);
esp_err_t bsp_tab5_audio_set_volume(int volume);   /*!< 0..100, 0 mutes */
esp_err_t bsp_tab5_audio_set_mute(bool mute);
int       bsp_tab5_audio_get_volume(void);

/* Speaker EQ — applied in-place inside bsp_tab5_audio_write. */
esp_err_t bsp_tab5_audio_eq_set_enabled(bool enabled);
bool      bsp_tab5_audio_eq_is_enabled(void);
esp_err_t bsp_tab5_audio_eq_set_biquads(const audio_eq_biquad_t *biquads, size_t num_stages);
audio_eq_t bsp_tab5_audio_eq_handle(void);  /*!< NULL if EQ was not initialised */

/* Speaker amp gate (PI4IOE1 pin 1) — read HP_DET via PI4IOE1 pin 7. */
esp_err_t bsp_tab5_audio_set_speaker_mode(bsp_speaker_mode_t mode);
bsp_speaker_mode_t bsp_tab5_audio_get_speaker_mode(void);
bool      bsp_tab5_audio_headphone_inserted(void);

/* Stereo→mono downmix for speaker output (only L wired on Tab5). */
esp_err_t bsp_tab5_audio_set_mono_mix(bool enabled);
bool      bsp_tab5_audio_get_mono_mix(void);

/* Headphone insert/remove notification.
 * Fires from the internal poller task whenever HP_DET changes (~200 ms granularity).
 * Pass NULL to unregister. Only one callback at a time. */
typedef void (*bsp_headphone_cb_t)(bool inserted, void *user);
esp_err_t bsp_tab5_audio_set_headphone_callback(bsp_headphone_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
