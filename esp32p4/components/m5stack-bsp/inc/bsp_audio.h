/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Tab5 speaker/audio API (ES8388 + software EQ). This is board-specific and has
 * no cross-model contract yet, so it keeps the bsp_tab5_audio_* names and lives
 * apart from the generic bsp.h surface. Implemented in boards/tab5/.
 */

#pragma once
#include "bsp.h"          /* bsp_speaker_mode_t */
#include "audio_eq.h"

#ifdef __cplusplus
extern "C" {
#endif

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
