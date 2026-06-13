/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Model-agnostic public BSP API. bsp_init() dispatches to the board selected at
 * build time (see boards/<model>/); within a board the panel generation is
 * resolved by the board itself. Audio is capability-based (bsp_audio_get_caps):
 * a model exposes only what its hardware has — PCM playback, a tone-only
 * buzzer, a speaker route, a headphone route — and calls outside the model's
 * capabilities return ESP_ERR_NOT_SUPPORTED.
 */

#pragma once
#include "bsp_types.h"
#include "audio_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BSP_AUDIO_CAP_PCM       = 1 << 0,  /*!< PCM playback path (DAC / codec / I2S amp) */
    BSP_AUDIO_CAP_TONE      = 1 << 1,  /*!< tone-only buzzer */
    BSP_AUDIO_CAP_SPEAKER   = 1 << 2,  /*!< speaker route */
    BSP_AUDIO_CAP_HEADPHONE = 1 << 3,  /*!< headphone route with insert detect */
} bsp_audio_caps_t;

typedef enum {
    BSP_AUDIO_SPEAKER_MODE_ON   = 0,  /*!< speaker always on (default — matches zero-init) */
    BSP_AUDIO_SPEAKER_MODE_AUTO = 1,  /*!< speaker on only while HP jack is unplugged (needs CAP_HEADPHONE) */
    BSP_AUDIO_SPEAKER_MODE_OFF  = 2,  /*!< speaker always off */
} bsp_audio_speaker_mode_t;

/* The DSP chain (see audio_dsp.h) exists to voice the board's own output path
 * (speaker correction EQ, mono-wired downmix), so its tuning belongs to the
 * board. This mode picks who drives it. In every mode but DISABLE, user
 * volume/mute are delivered through the chain's fading gain stage so
 * amplitude never steps (click-free). */
typedef enum {
    BSP_AUDIO_DSP_MODE_AUTO    = 0,  /*!< board tuning, auto-switched on route changes
                                          (e.g. speaker EQ+monomix vs headphone EQ on
                                          HP insert/remove) (default — matches zero-init) */
    BSP_AUDIO_DSP_MODE_MANUAL  = 1,  /*!< DSP initialised flat; the app drives it via
                                          bsp_audio_dsp() */
    BSP_AUDIO_DSP_MODE_DISABLE = 2,  /*!< no DSP; bsp_audio_dsp() == NULL, volume falls
                                          back to the hardware codec (clicky) */
} bsp_audio_dsp_mode_t;

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
        bool enable;
    } bluetooth;
    struct {
        bsp_audio_dsp_mode_t dsp_mode;            /*!< Who voices the DSP chain */
        bsp_audio_speaker_mode_t speaker_mode;    /*!< Speaker route policy */
    } audio;
} bsp_config_t;

esp_err_t bsp_init(const bsp_config_t *config);
void bsp_restart(void);

// MARK: USB
/* Switch the USB host port's VBUS (5V) rail on/off (on at boot per
 * bsp_config usb.usb5v_en). No-op on models without a switchable host-port rail
 * (e.g. the simulator). */
void bsp_usb_host_set_power(bool on);

// MARK: Display
bsp_pixel_format_t bsp_display_get_pixel_format(void);
void  bsp_display_set_brightness(int brightness);
void *bsp_display_get_frame_buffer(int fb_index);
void  bsp_display_flush(int fb_index);

// MARK: Touch
int   bsp_touch_read(bsp_touch_point_t *points, uint8_t max_points);
void  bsp_touch_wait_interrupt(void);

// MARK: Audio
/* BSP_AUDIO_CAP_* bits of the active provider; 0 = this model has no audio. */
uint32_t bsp_audio_get_caps(void);

/* PCM playback (CAP_PCM). bsp_init() only initialises the DAC/DSP — no signal
 * is output until open() starts the stream (the format is open's argument).
 * Volume and mute go through the software gain fade, so they are click-free;
 * open/reconfig fade the stream in from silence. write/reconfig/close before
 * open return ESP_ERR_INVALID_STATE. */
esp_err_t bsp_audio_open(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
esp_err_t bsp_audio_close(void);
esp_err_t bsp_audio_reconfig(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
/* `data` is filtered in-place by the DSP chain — caller must own the buffer.
 * Blocks while the output is full (the device's natural pacing). */
esp_err_t bsp_audio_write(void *data, size_t len);
/* 0..150, linear-in-dB; 0 is true silence, 100 = 0 dB (unity). 1..100 attenuates
 * (vol=1 → -40 dB); 100..150 is a digital boost above unity (vol=150 → +6 dB),
 * available only on the SW-gain (DSP) path — the HW-codec fallback caps at 100.
 * Starts at 0 until first set. Callable before open — the stream fades in to the
 * stored volume. */
esp_err_t bsp_audio_set_volume(int volume);
int       bsp_audio_get_volume(void);
esp_err_t bsp_audio_set_mute(bool mute);
bool      bsp_audio_get_mute(void);
/* The DSP chain handle, driven with the audio_dsp_* API directly. NULL in
 * DSP_MODE_DISABLE (or no PCM path). Note set_gain is owned by the
 * volume/mute plumbing, and in DSP_MODE_AUTO the board re-voices
 * biquads/eq-enable/mono-mix on route changes (direct audio_dsp_* edits get
 * overwritten — use bsp_audio_set_eq_enabled below for an EQ toggle that
 * survives re-voicing). */
audio_dsp_t bsp_audio_dsp(void);

/* App-controlled EQ enable. Unlike a direct audio_dsp_set_eq_enabled(), this is
 * remembered as an override so a DSP_MODE_AUTO route re-voicing (HP insert/remove)
 * keeps the app's choice instead of restoring the board profile's eq_enabled.
 * ESP_ERR_NOT_SUPPORTED when there is no DSP (DISABLE mode / no PCM path).
 * get returns the live EQ enable state (false when there is no DSP). */
esp_err_t bsp_audio_set_eq_enabled(bool enabled);
bool      bsp_audio_get_eq_enabled(void);

/* Speaker route policy (CAP_SPEAKER; AUTO additionally needs CAP_HEADPHONE). */
esp_err_t bsp_audio_set_speaker_mode(bsp_audio_speaker_mode_t mode);
bsp_audio_speaker_mode_t bsp_audio_get_speaker_mode(void);

/* Headphone detect (CAP_HEADPHONE). The callback fires from an internal poller
 * task whenever the insert state changes (~200 ms granularity). Pass NULL to
 * unregister. Only one callback at a time. */
bool bsp_audio_headphone_inserted(void);
typedef void (*bsp_audio_headphone_cb_t)(bool inserted, void *user);
esp_err_t bsp_audio_set_headphone_callback(bsp_audio_headphone_cb_t cb, void *user);

/* Buzzer (CAP_TONE). duration_ms=0 plays until bsp_audio_tone_stop(). */
esp_err_t bsp_audio_tone(uint32_t freq_hz, uint32_t duration_ms);
esp_err_t bsp_audio_tone_stop(void);

#ifdef __cplusplus
}
#endif
