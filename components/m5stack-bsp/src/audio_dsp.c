/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "audio_dsp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define DSP_DEFAULT_MAX_STAGES 5
/* Gain ceiling. >1.0 amplifies (the output is saturated to int16 in process);
 * +12 dB is a sane safety bound well above the BSP volume curve's +6 dB boost. */
#define DSP_MAX_GAIN           4.0f
/* process() snapshots the coefficients onto the stack up to this many stages
 * (beyond it, the in-place pointer path is used — see audio_dsp_process). */
#define DSP_SNAPSHOT_STAGES    8

typedef struct {
    float z1, z2;
} biquad_state_t;

struct audio_dsp_state {
    SemaphoreHandle_t mutex;
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  bits_per_sample;
    size_t   max_stages;
    size_t   num_stages;
    audio_dsp_biquad_t *biquads;       /* [max_stages] */
    biquad_state_t     *states;        /* [max_stages * channels] */
    bool eq_enabled;
    /* Software gain with per-sample fade. Applied AFTER biquads so changing
     * the gain doesn't disturb the filter memory. */
    float    current_gain;             /* per-frame interpolation cursor */
    float    target_gain;
    float    gain_step;                /* per-frame delta toward target */
    uint32_t fade_remaining;           /* frames left to step */
    bool     mono_mix;                 /* (L+R)/2 → both channels (stereo only) */
};

static inline float biquad_step(const audio_dsp_biquad_t *b, biquad_state_t *s, float x) {
    float y = b->b0 * x + s->z1;
    s->z1 = b->b1 * x - b->a1 * y + s->z2;
    s->z2 = b->b2 * x - b->a2 * y;
    return y;
}

static void reset_states(audio_dsp_t dsp) {
    if (dsp->states) {
        memset(dsp->states, 0, dsp->max_stages * dsp->channels * sizeof(biquad_state_t));
    }
}

esp_err_t audio_dsp_init(const audio_dsp_config_t *config, audio_dsp_t *out_dsp) {
    if (!config || !out_dsp) return ESP_ERR_INVALID_ARG;
    if (config->channels < 1 || config->channels > 2) return ESP_ERR_INVALID_ARG;
    if (config->bits_per_sample != 16) return ESP_ERR_INVALID_ARG;
    if (!config->sample_rate) return ESP_ERR_INVALID_ARG;

    size_t initial_n = config->biquads ? config->num_stages : 0;
    size_t max_stages = config->max_stages;
    if (!max_stages) {
        max_stages = initial_n > DSP_DEFAULT_MAX_STAGES ? initial_n : DSP_DEFAULT_MAX_STAGES;
    }
    if (initial_n > max_stages) return ESP_ERR_INVALID_ARG;

    audio_dsp_t dsp = calloc(1, sizeof(*dsp));
    if (!dsp) return ESP_ERR_NO_MEM;

    dsp->mutex = xSemaphoreCreateMutex();
    dsp->biquads = calloc(max_stages, sizeof(audio_dsp_biquad_t));
    dsp->states  = calloc(max_stages * config->channels, sizeof(biquad_state_t));
    if (!dsp->mutex || !dsp->biquads || !dsp->states) {
        audio_dsp_deinit(dsp);
        return ESP_ERR_NO_MEM;
    }

    dsp->sample_rate     = config->sample_rate;
    dsp->channels        = config->channels;
    dsp->bits_per_sample = config->bits_per_sample;
    dsp->max_stages      = max_stages;
    dsp->num_stages      = initial_n;
    dsp->eq_enabled      = config->eq_enabled;
    dsp->current_gain    = 1.0f;
    dsp->target_gain     = 1.0f;
    dsp->gain_step       = 0.0f;
    dsp->fade_remaining  = 0;
    dsp->mono_mix        = config->mono_mix;
    if (initial_n) memcpy(dsp->biquads, config->biquads, initial_n * sizeof(audio_dsp_biquad_t));

    *out_dsp = dsp;
    return ESP_OK;
}

esp_err_t audio_dsp_deinit(audio_dsp_t dsp) {
    if (!dsp) return ESP_ERR_INVALID_ARG;
    if (dsp->mutex) vSemaphoreDelete(dsp->mutex);
    free(dsp->biquads);
    free(dsp->states);
    free(dsp);
    return ESP_OK;
}

esp_err_t audio_dsp_set_biquads(audio_dsp_t dsp, const audio_dsp_biquad_t *biquads, size_t num_stages) {
    if (!dsp) return ESP_ERR_INVALID_ARG;
    if (num_stages && !biquads) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(dsp->mutex, portMAX_DELAY);
    if (num_stages > dsp->max_stages) {
        /* Grow the stage capacity to fit. States are recreated (they are
         * reset below anyway); coefficients are fully rewritten below. */
        audio_dsp_biquad_t *new_biquads = calloc(num_stages, sizeof(audio_dsp_biquad_t));
        biquad_state_t *new_states = calloc(num_stages * dsp->channels, sizeof(biquad_state_t));
        if (!new_biquads || !new_states) {
            free(new_biquads);
            free(new_states);
            xSemaphoreGive(dsp->mutex);
            return ESP_ERR_NO_MEM;
        }
        free(dsp->biquads);
        free(dsp->states);
        dsp->biquads    = new_biquads;
        dsp->states     = new_states;
        dsp->max_stages = num_stages;
    }
    if (num_stages) memcpy(dsp->biquads, biquads, num_stages * sizeof(audio_dsp_biquad_t));
    dsp->num_stages = num_stages;
    reset_states(dsp);
    xSemaphoreGive(dsp->mutex);
    return ESP_OK;
}

esp_err_t audio_dsp_set_eq_enabled(audio_dsp_t dsp, bool enabled) {
    if (!dsp) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(dsp->mutex, portMAX_DELAY);
    if (enabled && !dsp->eq_enabled) reset_states(dsp);
    dsp->eq_enabled = enabled;
    xSemaphoreGive(dsp->mutex);
    return ESP_OK;
}

bool audio_dsp_is_eq_enabled(audio_dsp_t dsp) {
    return dsp && dsp->eq_enabled;
}

esp_err_t audio_dsp_reconfig(audio_dsp_t dsp, uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
    if (!dsp) return ESP_ERR_INVALID_ARG;
    if (channels < 1 || channels > 2) return ESP_ERR_INVALID_ARG;
    if (bits_per_sample != 16) return ESP_ERR_INVALID_ARG;
    if (!sample_rate) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(dsp->mutex, portMAX_DELAY);
    if (channels != dsp->channels) {
        biquad_state_t *new_states = calloc(dsp->max_stages * channels, sizeof(biquad_state_t));
        if (!new_states) {
            xSemaphoreGive(dsp->mutex);
            return ESP_ERR_NO_MEM;
        }
        free(dsp->states);
        dsp->states = new_states;
    }
    dsp->sample_rate     = sample_rate;
    dsp->channels        = channels;
    dsp->bits_per_sample = bits_per_sample;
    reset_states(dsp);
    xSemaphoreGive(dsp->mutex);
    return ESP_OK;
}

esp_err_t audio_dsp_process(audio_dsp_t dsp, void *data, size_t bytes) {
    if (!dsp || !data) return ESP_ERR_INVALID_ARG;

    /* Snapshot {biquads, gain} under a short lock, then process the buffer
     * lock-free. Holding the mutex across the full per-sample loop starves
     * concurrent set_gain / set_biquads callers (UI thread) and was observed
     * to stall higher-priority work on the same core. Biquad states are only
     * touched by this function (single consumer), so they're safe to read /
     * write outside the lock. */
    audio_dsp_biquad_t local_biquads[DSP_SNAPSHOT_STAGES];
    audio_dsp_biquad_t *biquads_ptr;
    size_t   n_stages;
    uint8_t  channels;
    bool     do_biquads;
    bool     mono_mix;
    float    current_gain, target_gain, gain_step;
    uint32_t fade_remaining;
    biquad_state_t *states;

    xSemaphoreTake(dsp->mutex, portMAX_DELAY);
    n_stages       = dsp->num_stages;
    channels       = dsp->channels;
    do_biquads     = dsp->eq_enabled && n_stages > 0;
    mono_mix       = dsp->mono_mix && channels == 2;
    current_gain   = dsp->current_gain;
    target_gain    = dsp->target_gain;
    gain_step      = dsp->gain_step;
    fade_remaining = dsp->fade_remaining;
    states         = dsp->states;
    /* Cheap to copy: up to DSP_SNAPSHOT_STAGES * 5 floats = 160 bytes. Larger
     * configs keep the mutex held across the processing loop instead — the
     * direct pointer would dangle if a concurrent set_biquads grows (and thus
     * reallocates) the stage arrays. Coefficient updates that large are rare,
     * so the starvation tradeoff only applies where it can't be helped. */
    bool hold_lock = n_stages > DSP_SNAPSHOT_STAGES;
    if (!hold_lock) {
        if (n_stages) memcpy(local_biquads, dsp->biquads, n_stages * sizeof(audio_dsp_biquad_t));
        biquads_ptr = local_biquads;
        xSemaphoreGive(dsp->mutex);
    } else {
        biquads_ptr = dsp->biquads;
    }

    const bool apply_gain = (current_gain != 1.0f) || (target_gain != 1.0f) || (fade_remaining > 0);
    if (!do_biquads && !apply_gain && !mono_mix) {
        if (hold_lock) xSemaphoreGive(dsp->mutex);
        return ESP_OK;
    }

    const size_t frame_bytes = sizeof(int16_t) * channels;
    if (bytes % frame_bytes) {
        if (hold_lock) xSemaphoreGive(dsp->mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t frames = bytes / frame_bytes;

    int16_t *p = (int16_t *)data;
    for (size_t f = 0; f < frames; f++) {
        /* Per-frame gain step (same multiplier for L+R so stereo image holds). */
        if (fade_remaining > 0) {
            current_gain += gain_step;
            if (--fade_remaining == 0) current_gain = target_gain;
        }
        const float g = current_gain;

        float ys[2];
        for (uint8_t ch = 0; ch < channels; ch++) {
            float x = (float)p[f * channels + ch];
            if (do_biquads) {
                for (size_t s = 0; s < n_stages; s++) {
                    x = biquad_step(&biquads_ptr[s], &states[s * channels + ch], x);
                }
            }
            x *= g;
            ys[ch] = x;
        }
        if (mono_mix) {
            const float mono = (ys[0] + ys[1]) * 0.5f;
            ys[0] = mono;
            ys[1] = mono;
        }
        for (uint8_t ch = 0; ch < channels; ch++) {
            float x = ys[ch];
            if (x >  32767.0f) x =  32767.0f;
            if (x < -32768.0f) x = -32768.0f;
            p[f * channels + ch] = (int16_t)lrintf(x);
        }
    }

    /* Publish the advanced gain cursor back so the next buffer continues the
     * fade. set_gain races are benign: if a new target was set while we were
     * processing, our writeback below is conditional so we don't overwrite it. */
    if (!hold_lock) xSemaphoreTake(dsp->mutex, portMAX_DELAY);
    /* Only update if no one else changed the fade meanwhile. */
    if (dsp->target_gain == target_gain) {
        dsp->current_gain   = current_gain;
        dsp->fade_remaining = fade_remaining;
    }
    xSemaphoreGive(dsp->mutex);
    return ESP_OK;
}

esp_err_t audio_dsp_set_gain(audio_dsp_t dsp, float target_gain, uint32_t fade_ms) {
    if (!dsp) return ESP_ERR_INVALID_ARG;
    if (target_gain < 0.0f)          target_gain = 0.0f;
    if (target_gain > DSP_MAX_GAIN)  target_gain = DSP_MAX_GAIN;

    xSemaphoreTake(dsp->mutex, portMAX_DELAY);
    dsp->target_gain = target_gain;
    if (fade_ms == 0 || target_gain == dsp->current_gain) {
        dsp->current_gain   = target_gain;
        dsp->fade_remaining = 0;
        dsp->gain_step      = 0.0f;
    } else {
        uint32_t frames = (uint32_t)(((uint64_t)dsp->sample_rate * fade_ms + 500) / 1000);
        if (frames == 0) frames = 1;
        dsp->fade_remaining = frames;
        dsp->gain_step      = (target_gain - dsp->current_gain) / (float)frames;
    }
    xSemaphoreGive(dsp->mutex);
    return ESP_OK;
}

float audio_dsp_get_gain(audio_dsp_t dsp) {
    return dsp ? dsp->target_gain : 0.0f;
}

esp_err_t audio_dsp_set_mono_mix(audio_dsp_t dsp, bool enabled) {
    if (!dsp) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(dsp->mutex, portMAX_DELAY);
    dsp->mono_mix = enabled;
    xSemaphoreGive(dsp->mutex);
    return ESP_OK;
}

bool audio_dsp_get_mono_mix(audio_dsp_t dsp) {
    return dsp && dsp->mono_mix;
}

/* ---------------- RBJ Cookbook designers ---------------- */

static inline audio_dsp_biquad_t normalize(float b0, float b1, float b2, float a0, float a1, float a2) {
    float inv = 1.0f / a0;
    return (audio_dsp_biquad_t){
        .b0 = b0 * inv, .b1 = b1 * inv, .b2 = b2 * inv,
        .a1 = a1 * inv, .a2 = a2 * inv,
    };
}

audio_dsp_biquad_t audio_dsp_design_peaking(uint32_t fs, float f0, float q, float gain_db) {
    float A      = powf(10.0f, gain_db / 40.0f);
    float w0     = 2.0f * (float)M_PI * f0 / (float)fs;
    float cos_w0 = cosf(w0);
    float alpha  = sinf(w0) / (2.0f * q);
    return normalize(
        1.0f + alpha * A,
       -2.0f * cos_w0,
        1.0f - alpha * A,
        1.0f + alpha / A,
       -2.0f * cos_w0,
        1.0f - alpha / A);
}

audio_dsp_biquad_t audio_dsp_design_low_shelf(uint32_t fs, float f0, float q, float gain_db) {
    float A      = powf(10.0f, gain_db / 40.0f);
    float w0     = 2.0f * (float)M_PI * f0 / (float)fs;
    float cos_w0 = cosf(w0);
    float alpha  = sinf(w0) / (2.0f * q);
    float beta   = 2.0f * sqrtf(A) * alpha;
    return normalize(
            A * ((A + 1.0f) - (A - 1.0f) * cos_w0 + beta),
     2.0f * A * ((A - 1.0f) - (A + 1.0f) * cos_w0),
            A * ((A + 1.0f) - (A - 1.0f) * cos_w0 - beta),
                (A + 1.0f) + (A - 1.0f) * cos_w0 + beta,
        -2.0f * ((A - 1.0f) + (A + 1.0f) * cos_w0),
                (A + 1.0f) + (A - 1.0f) * cos_w0 - beta);
}

audio_dsp_biquad_t audio_dsp_design_high_shelf(uint32_t fs, float f0, float q, float gain_db) {
    float A      = powf(10.0f, gain_db / 40.0f);
    float w0     = 2.0f * (float)M_PI * f0 / (float)fs;
    float cos_w0 = cosf(w0);
    float alpha  = sinf(w0) / (2.0f * q);
    float beta   = 2.0f * sqrtf(A) * alpha;
    return normalize(
             A * ((A + 1.0f) + (A - 1.0f) * cos_w0 + beta),
     -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cos_w0),
             A * ((A + 1.0f) + (A - 1.0f) * cos_w0 - beta),
                 (A + 1.0f) - (A - 1.0f) * cos_w0 + beta,
         2.0f * ((A - 1.0f) - (A + 1.0f) * cos_w0),
                 (A + 1.0f) - (A - 1.0f) * cos_w0 - beta);
}

audio_dsp_biquad_t audio_dsp_design_lowpass(uint32_t fs, float f0, float q) {
    float w0     = 2.0f * (float)M_PI * f0 / (float)fs;
    float cos_w0 = cosf(w0);
    float alpha  = sinf(w0) / (2.0f * q);
    float k      = (1.0f - cos_w0) * 0.5f;
    return normalize(
        k, 2.0f * k, k,
        1.0f + alpha, -2.0f * cos_w0, 1.0f - alpha);
}

audio_dsp_biquad_t audio_dsp_design_highpass(uint32_t fs, float f0, float q) {
    float w0     = 2.0f * (float)M_PI * f0 / (float)fs;
    float cos_w0 = cosf(w0);
    float alpha  = sinf(w0) / (2.0f * q);
    float k      = (1.0f + cos_w0) * 0.5f;
    return normalize(
        k, -2.0f * k, k,
        1.0f + alpha, -2.0f * cos_w0, 1.0f - alpha);
}
