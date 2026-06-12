/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Internal audio-driver interface. A provider allocates a struct whose FIRST
 * member is a bsp_audio_t (struct-inheritance vtable, like bsp_display.h),
 * fills `caps` and the function pointers its hardware supports (the rest stay
 * NULL), and returns &state->base from its *_create(). The shared dispatch
 * (src/bsp_audio.c) implements the public bsp_audio_* API on top: capability
 * gating, the user volume curve (delivered as a fading software gain through
 * an audio_dsp instance it owns), the DSP voicing mode (Auto/Manual/Disable —
 * Auto pulls the board's tuning from get_dsp_profile and re-applies it on
 * route changes), the speaker route policy (ON/AUTO/OFF + the headphone poll
 * task), and the click-free sequencing. A provider only does low-level
 * hardware ops — it never implements policy.
 *
 * Lifecycle: a provider registers CLOSED — DAC powered down/muted, no signal
 * output (bus clocks may idle); the amp gate stays off. The first public
 * open() powers the stream and runs the bring-up ordering (silent SW gain →
 * codec unmute at max HW volume → analog settle → amp on per speaker_mode);
 * close() stops the stream but keeps the amp per route policy (the DAC is
 * hw-muted first, so the silence holds and the amp transient isn't re-paid on
 * the next open).
 *
 * Click-free contract a provider relies on (and must not break itself):
 *   1. Amp / speaker-route state (set_speaker_enabled) is only changed while
 *      the DAC output is settled silence.
 *   2. Audible amplitude changes always go through the software gain fade —
 *      set_hw_volume / set_hw_mute are reserved for power transitions where
 *      the output is already silent (or about to lose its clocks).
 */

#pragma once
#include "bsp.h"

typedef struct bsp_audio bsp_audio_t;

/* A board's DSP tuning for one output route, designed at `sample_rate` (the
 * dispatch re-queries on open/reconfig and — in DSP_MODE_AUTO — on headphone
 * insert/remove). Filled into caller storage so the board needs no statics. */
#define BSP_AUDIO_DSP_PROFILE_MAX_STAGES 8
typedef struct {
    audio_dsp_biquad_t biquads[BSP_AUDIO_DSP_PROFILE_MAX_STAGES];
    size_t num_stages;
    bool eq_enabled;
    bool mono_mix;
} bsp_audio_dsp_profile_t;

struct bsp_audio {
    uint32_t caps;   /* BSP_AUDIO_CAP_* */

    /* PCM playback — required when CAP_PCM is set, NULL otherwise.
     * open() starts signal output at the given format; close() stops it.
     * write() blocks while the output pipeline is full (I2S DMA / host queue);
     * that backpressure is the public API's pacing. */
    esp_err_t (*open)(bsp_audio_t *self, uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
    esp_err_t (*close)(bsp_audio_t *self);
    esp_err_t (*reconfig)(bsp_audio_t *self, uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
    esp_err_t (*write)(bsp_audio_t *self, const void *data, size_t len);

    /* Hardware volume/mute — optional (NULL when the codec has none, or when
     * volume is software-gain only). Power-transition use only, see above. */
    esp_err_t (*set_hw_volume)(bsp_audio_t *self, int volume);   /* 0..100 */
    esp_err_t (*set_hw_mute)(bsp_audio_t *self, bool mute);

    /* Routing — NULL when the hardware has no such control. */
    esp_err_t (*set_speaker_enabled)(bsp_audio_t *self, bool enabled);     /* amp gate */
    esp_err_t (*headphone_inserted)(bsp_audio_t *self, bool *inserted);    /* CAP_HEADPHONE */

    /* Board DSP voicing per route — optional (NULL = flat in every mode). */
    esp_err_t (*get_dsp_profile)(bsp_audio_t *self, bool headphone, uint32_t sample_rate,
                                 bsp_audio_dsp_profile_t *out);

    /* Buzzer — required when CAP_TONE is set, NULL otherwise. */
    esp_err_t (*tone)(bsp_audio_t *self, uint32_t freq_hz, uint32_t duration_ms);
    esp_err_t (*tone_stop)(bsp_audio_t *self);

    esp_err_t (*deinit)(bsp_audio_t *self);
};

/* Dispatch-layer policy selection, from bsp_config_t.audio. */
typedef struct {
    bsp_audio_dsp_mode_t dsp_mode;
    bsp_audio_speaker_mode_t speaker_mode;
} bsp_audio_init_t;

/* Register the active audio provider with the common layer (src/bsp_audio.c).
 * A board's bsp_init() calls this once after creating its (closed) provider;
 * no signal is output and the amp gate is left off until the first public
 * open(). NULL deactivates audio. */
void bsp_audio_set_active(bsp_audio_t *audio, const bsp_audio_init_t *init);

/* Silence the audio path for a power transition (hw mute + amp off) — called
 * by bsp_restart() before the I2S clocks die. The caller owns the settle
 * delay before cutting power. Safe with no active provider. */
void bsp_audio_quiesce(void);
