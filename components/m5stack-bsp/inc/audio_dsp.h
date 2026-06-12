/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Software audio post-processing for fixed-point PCM streams, shared by every
 * board with a PCM playback path (the bsp_audio dispatch layer owns one
 * instance and runs it inside bsp_audio_write). The chain is fixed:
 *
 *   EQ (cascaded biquads) -> gain (with per-sample fade) -> mono mix
 *
 * and each stage can be enabled/disabled independently (a fully bypassed
 * chain returns without touching the buffer). The gain fade is the BSP's
 * click-free volume/mute primitive: amplitude never steps, it ramps
 * frame-by-frame inside audio_dsp_process.
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
} audio_dsp_biquad_t;

typedef struct audio_dsp_state *audio_dsp_t;

typedef struct {
    uint32_t sample_rate;
    uint8_t  channels;                 /*!< 1 or 2 — interleaved */
    uint8_t  bits_per_sample;          /*!< 16 supported */
    size_t   max_stages;               /*!< initial capacity; 0 -> max(num_stages, 5).
                                            Grows automatically in set_biquads. */
    bool     eq_enabled;               /*!< initial EQ enable state */
    const audio_dsp_biquad_t *biquads; /*!< initial coefficients (copied) */
    size_t   num_stages;
    bool     mono_mix;                 /*!< initial mono-mix enable state */
} audio_dsp_config_t;

esp_err_t audio_dsp_init(const audio_dsp_config_t *config, audio_dsp_t *dsp);
esp_err_t audio_dsp_deinit(audio_dsp_t dsp);

/* Reconfigure for a new sample rate / channel count. Resets filter state. */
esp_err_t audio_dsp_reconfig(audio_dsp_t dsp, uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);

/* In-place processing of `bytes` of interleaved PCM samples. Returns without
 * touching the buffer when every stage is bypassed (EQ off, gain at unity with
 * no fade in flight, mono mix off). */
esp_err_t audio_dsp_process(audio_dsp_t dsp, void *data, size_t bytes);

/* EQ stage. Replacing coefficients resets filter state; num_stages=0 clears.
 * The stage capacity grows automatically when num_stages exceeds it. */
esp_err_t audio_dsp_set_eq_enabled(audio_dsp_t dsp, bool enabled);
bool      audio_dsp_is_eq_enabled(audio_dsp_t dsp);
esp_err_t audio_dsp_set_biquads(audio_dsp_t dsp, const audio_dsp_biquad_t *biquads, size_t num_stages);

/* Gain stage. target_gain is linear; 1.0 = bypass (unity). >1.0 amplifies (the
 * output is saturated to int16 in process) up to a +12 dB ceiling; <1.0
 * attenuates; 0.0 = silence. fade_ms=0 applies instantly; otherwise the gain is
 * interpolated frame-by-frame so a zipper-free transition happens inside
 * audio_dsp_process. The gain applies regardless of the EQ enable flag. */
esp_err_t audio_dsp_set_gain(audio_dsp_t dsp, float target_gain, uint32_t fade_ms);
float     audio_dsp_get_gain(audio_dsp_t dsp);   /*!< target (not interpolated) */

/* Mono-mix stage: stereo→mono downmix after gain. When enabled, both output
 * channels carry (L+R)/2 so a mono-wired speaker hears content from both
 * incoming channels. No-op on mono streams. */
esp_err_t audio_dsp_set_mono_mix(audio_dsp_t dsp, bool enabled);
bool      audio_dsp_get_mono_mix(audio_dsp_t dsp);

/* RBJ cookbook designers — f0 in Hz, gain_db only used for peaking/shelf. */
audio_dsp_biquad_t audio_dsp_design_peaking   (uint32_t fs, float f0, float q, float gain_db);
audio_dsp_biquad_t audio_dsp_design_low_shelf (uint32_t fs, float f0, float q, float gain_db);
audio_dsp_biquad_t audio_dsp_design_high_shelf(uint32_t fs, float f0, float q, float gain_db);
audio_dsp_biquad_t audio_dsp_design_lowpass   (uint32_t fs, float f0, float q);
audio_dsp_biquad_t audio_dsp_design_highpass  (uint32_t fs, float f0, float q);

#ifdef __cplusplus
}
#endif
