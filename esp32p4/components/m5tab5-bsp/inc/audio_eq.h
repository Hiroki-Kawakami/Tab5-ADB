/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Cascaded biquad EQ for fixed-point PCM streams.
 *
 * Filters are normalised to a0=1 and evaluated with Direct-Form II Transposed:
 *   y[n] = b0*x[n] + z1
 *   z1   = b1*x[n] - a1*y[n] + z2
 *   z2   = b2*x[n] - a2*y[n]
 *
 * Designer helpers follow the RBJ "Audio EQ Cookbook" formulae.
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float b0, b1, b2;
    float a1, a2;
} audio_eq_biquad_t;

typedef struct audio_eq_state *audio_eq_t;

typedef struct {
    uint32_t sample_rate;
    uint8_t  channels;                 /*!< 1 or 2 — interleaved */
    uint8_t  bits_per_sample;          /*!< 16 supported */
    size_t   max_stages;               /*!< 0 -> max(initial_num_stages, 8) */
    bool     enabled;                  /*!< initial enable state */
    const audio_eq_biquad_t *initial_biquads;
    size_t   initial_num_stages;
} audio_eq_config_t;

esp_err_t audio_eq_init(const audio_eq_config_t *config, audio_eq_t *eq);
esp_err_t audio_eq_deinit(audio_eq_t eq);

/* Replace coefficients and reset filter state. num_stages=0 clears. */
esp_err_t audio_eq_set_biquads(audio_eq_t eq, const audio_eq_biquad_t *biquads, size_t num_stages);

esp_err_t audio_eq_set_enabled(audio_eq_t eq, bool enabled);
bool      audio_eq_is_enabled(audio_eq_t eq);

/* Reconfigure for a new sample rate / channel count. Resets state. */
esp_err_t audio_eq_reconfig(audio_eq_t eq, uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);

/* In-place processing of `bytes` of interleaved PCM samples. No-op when disabled
 * AND gain is unity. The software gain (see audio_eq_set_gain) is applied
 * regardless of the enabled flag, so a paused-EQ fader still works. */
esp_err_t audio_eq_process(audio_eq_t eq, void *data, size_t bytes);

/* Software output gain with per-sample fade. target_gain is linear (0.0..1.0).
 * fade_ms=0 applies instantly; otherwise the gain is interpolated frame-by-
 * frame so a zipper-free transition happens inside audio_eq_process. */
esp_err_t audio_eq_set_gain(audio_eq_t eq, float target_gain, uint32_t fade_ms);
float     audio_eq_get_gain(audio_eq_t eq);   /*!< target (not interpolated) */

/* Stereo→mono downmix after gain. When enabled, both output channels carry
 * (L+R)/2 so a mono-wired speaker hears content from both incoming channels. */
esp_err_t audio_eq_set_mono_mix(audio_eq_t eq, bool enabled);
bool      audio_eq_get_mono_mix(audio_eq_t eq);

/* RBJ cookbook designers — f0 in Hz, gain_db only used for peaking/shelf. */
audio_eq_biquad_t audio_eq_design_peaking   (uint32_t fs, float f0, float q, float gain_db);
audio_eq_biquad_t audio_eq_design_low_shelf (uint32_t fs, float f0, float q, float gain_db);
audio_eq_biquad_t audio_eq_design_high_shelf(uint32_t fs, float f0, float q, float gain_db);
audio_eq_biquad_t audio_eq_design_lowpass   (uint32_t fs, float f0, float q);
audio_eq_biquad_t audio_eq_design_highpass  (uint32_t fs, float f0, float q);

#ifdef __cplusplus
}
#endif
