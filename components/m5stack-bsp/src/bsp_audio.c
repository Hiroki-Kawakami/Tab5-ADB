/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Model-agnostic audio layer: holds the active bsp_audio provider (registered
 * by the board's bsp_init via bsp_audio_set_active) and implements the public
 * bsp_audio_* API by dispatching through its vtable. Owns everything that is
 * policy rather than hardware: capability gating, the user volume curve
 * (linear-in-dB, delivered as a fading software gain through an audio_dsp
 * instance), mute (a software fade — hardware mute is reserved for power
 * transitions), the DSP voicing mode (Auto: board profile re-applied on route
 * changes; Manual: flat init, app-driven; Disable: no DSP), the speaker route
 * policy (ON/AUTO/OFF + the headphone poll task + insert callback), and the
 * click-free open/close sequencing (see inc_private/bsp_audio.h).
 */

#include "bsp.h"
#include "bsp_audio.h"
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BSP_AUDIO";

#define BSP_VOLUME_FADE_MS  100
#define BSP_OPEN_FADE_MS    50
#define BSP_VOLUME_DB_SPAN  40.0f   /* vol=1 → -40 dB, vol=100 → 0 dB */
#define BSP_VOLUME_MAX      150     /* max accepted volume; 100..MAX amplifies */
#define BSP_VOLUME_BOOST_DB 6.0f    /* vol=BSP_VOLUME_MAX → +6 dB (≈2x amplitude) */
#define BSP_AMP_SETTLE_MS   50      /* DAC analog settle before touching the amp */
#define BSP_MUTE_SETTLE_MS  20      /* hw-mute settle before stopping clocks */
#define BSP_NOMINAL_RATE    48000   /* DSP placeholder rate until the first open */

static bsp_audio_t *s_audio;
static audio_dsp_t  s_dsp;
static bsp_audio_dsp_mode_t s_dsp_mode = BSP_AUDIO_DSP_MODE_AUTO;
static bool s_dsp_bypass;        /* stream format the DSP can't process (bits != 16) */
static bool s_open;              /* stream running (between open and close) */
static bool s_armed;             /* first open done → amp may follow route policy */
static uint32_t s_rate = BSP_NOMINAL_RATE;   /* current stream rate (profile design fs) */
/* User-facing volume tracked here; hardware codec volume is pinned to max at
 * open and the DSP applies the user value as a software gain (with fade). */
static int  s_volume = -1;       /* -1 = never set → silent */
static bool s_mute;
/* App override for the EQ enable. -1 = follow the board profile's eq_enabled
 * (DSP_MODE_AUTO default); 0/1 = the app forced it, which survives route
 * re-voicing (see apply_dsp_profile). */
static int  s_eq_override = -1;

static volatile bsp_audio_speaker_mode_t s_speaker_mode = BSP_AUDIO_SPEAKER_MODE_ON;
static TaskHandle_t s_route_task;
static portMUX_TYPE s_hp_mux = portMUX_INITIALIZER_UNLOCKED;
static bsp_audio_headphone_cb_t s_hp_cb;
static void *s_hp_cb_arg;
static bool s_hp_last;
static bool s_hp_last_valid;

static bool hp_inserted_now(void) {
    if (!s_audio || !s_audio->headphone_inserted) return false;
    bool hp = false;
    if (s_audio->headphone_inserted(s_audio, &hp) != ESP_OK) return false;
    return hp;
}

static void apply_speaker_with_hp(bsp_audio_speaker_mode_t mode, bool hp) {
    if (!s_audio || !s_audio->set_speaker_enabled) return;
    bool desired;
    switch (mode) {
        case BSP_AUDIO_SPEAKER_MODE_ON:   desired = true; break;
        case BSP_AUDIO_SPEAKER_MODE_AUTO: desired = !hp;  break;
        case BSP_AUDIO_SPEAKER_MODE_OFF:
        default:                          desired = false; break;
    }
    /* Until the first open the DAC has never produced settled silence, so the
     * amp stays off regardless of policy (the click-free contract). */
    s_audio->set_speaker_enabled(s_audio, desired && s_armed);
}

static void apply_speaker(bsp_audio_speaker_mode_t mode) {
    apply_speaker_with_hp(mode, hp_inserted_now());
}

/* DSP_MODE_AUTO: pull the board's tuning for the current route and apply it.
 * Callable from the app threads (open/reconfig) and the route task — the DSP
 * setters serialize internally. */
static void apply_dsp_profile(bool hp) {
    if (!s_dsp || s_dsp_mode != BSP_AUDIO_DSP_MODE_AUTO) return;
    if (!s_audio || !s_audio->get_dsp_profile) return;
    bsp_audio_dsp_profile_t profile = {0};
    if (s_audio->get_dsp_profile(s_audio, hp, s_rate, &profile) != ESP_OK) return;
    if (profile.num_stages > BSP_AUDIO_DSP_PROFILE_MAX_STAGES) return;
    audio_dsp_set_biquads(s_dsp, profile.biquads, profile.num_stages);
    /* App override wins over the board profile so an HP insert/remove re-voicing
     * doesn't clobber the user's EQ on/off choice. */
    audio_dsp_set_eq_enabled(s_dsp, s_eq_override >= 0 ? (bool)s_eq_override
                                                       : profile.eq_enabled);
    audio_dsp_set_mono_mix(s_dsp, profile.mono_mix);
}

static void route_task(void *arg) {
    (void)arg;
    while (1) {
        bsp_audio_speaker_mode_t mode = s_speaker_mode;
        bool hp = hp_inserted_now();

        /* Detect HP state change: re-voice the DSP for the new route (Auto)
         * and dispatch the user callback (fired outside the critical section
         * so user code can take its time / call into BSP). */
        bsp_audio_headphone_cb_t cb = NULL;
        void *cb_arg = NULL;
        bool changed = false;
        portENTER_CRITICAL(&s_hp_mux);
        if (s_hp_last_valid && hp != s_hp_last) {
            changed = true;
            cb = s_hp_cb;
            cb_arg = s_hp_cb_arg;
        }
        s_hp_last = hp;
        s_hp_last_valid = true;
        portEXIT_CRITICAL(&s_hp_mux);
        if (changed) {
            apply_dsp_profile(hp);
            if (cb) cb(hp, cb_arg);
        }

        apply_speaker_with_hp(mode, hp);

        bool dsp_auto = s_dsp && s_dsp_mode == BSP_AUDIO_DSP_MODE_AUTO &&
                        s_audio && s_audio->get_dsp_profile;
        bool need_poll = (mode == BSP_AUDIO_SPEAKER_MODE_AUTO) || (s_hp_cb != NULL) || dsp_auto;
        TickType_t wait = need_poll ? pdMS_TO_TICKS(200) : portMAX_DELAY;
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

static esp_err_t start_route_task_once(void) {
    if (s_route_task) return ESP_OK;
    return xTaskCreate(route_task, "bsp_audio_rt", 2048, NULL, 1, &s_route_task) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}

/* The route task is needed whenever something tracks the headphone state:
 * the AUTO speaker policy, a user insert callback, or Auto DSP voicing. */
static bool route_task_needed(void) {
    if (!s_audio || !(s_audio->caps & BSP_AUDIO_CAP_HEADPHONE)) return false;
    return s_speaker_mode == BSP_AUDIO_SPEAKER_MODE_AUTO || s_hp_cb ||
           (s_dsp && s_dsp_mode == BSP_AUDIO_DSP_MODE_AUTO && s_audio->get_dsp_profile);
}

static float volume_to_gain(int volume) {
    /* Linear-in-dB curve. vol<=0 is a hard zero so muting via volume=0 is true
     * silence. 1..100 attenuates (vol=1 → -40 dB, vol=100 → 0 dB / gain 1.0);
     * 100..MAX is a gentler boost above unity (vol=MAX → +BOOST dB, a digital
     * gain > 1 — useful when the source is quiet, capped to avoid clipping). */
    if (volume <= 0) return 0.0f;
    if (volume > BSP_VOLUME_MAX) volume = BSP_VOLUME_MAX;
    float db;
    if (volume <= 100)
        db = (volume - 100) * (BSP_VOLUME_DB_SPAN / 100.0f);
    else
        db = (volume - 100) * (BSP_VOLUME_BOOST_DB / (BSP_VOLUME_MAX - 100));
    return powf(10.0f, db / 20.0f);
}

static float current_target_gain(void) {
    return s_mute ? 0.0f : volume_to_gain(s_volume);
}

void bsp_audio_set_active(bsp_audio_t *audio, const bsp_audio_init_t *init) {
    if (s_dsp) {
        audio_dsp_deinit(s_dsp);
        s_dsp = NULL;
    }
    s_audio = audio;
    s_dsp_mode = BSP_AUDIO_DSP_MODE_AUTO;
    s_dsp_bypass = false;
    s_open = false;
    s_armed = false;
    s_rate = BSP_NOMINAL_RATE;
    s_volume = -1;
    s_mute = false;
    s_eq_override = -1;  /* follow the board profile until the app forces it */
    if (!audio) return;

    static const bsp_audio_init_t defaults = {0};
    if (!init) init = &defaults;
    s_dsp_mode = init->dsp_mode;
    s_speaker_mode = init->speaker_mode;

    if ((audio->caps & BSP_AUDIO_CAP_PCM) && s_dsp_mode != BSP_AUDIO_DSP_MODE_DISABLE) {
        /* Created flat at a nominal rate so bsp_audio_dsp() is valid from
         * boot; open() reconfigures to the real stream format. */
        audio_dsp_config_t dsp_cfg = {
            .sample_rate     = BSP_NOMINAL_RATE,
            .channels        = 2,
            .bits_per_sample = 16,
        };
        esp_err_t err = audio_dsp_init(&dsp_cfg, &s_dsp);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "audio_dsp_init failed: %d (DSP disabled)", err);
            s_dsp = NULL;
        } else {
            audio_dsp_set_gain(s_dsp, 0.0f, 0);
            apply_dsp_profile(hp_inserted_now());  /* Auto: board voicing from boot */
        }
    }

    if (route_task_needed()) {
        if (start_route_task_once() != ESP_OK) {
            ESP_LOGW(TAG, "route task start failed (HP tracking degraded)");
        }
    }
}

void bsp_audio_quiesce(void) {
    if (!s_audio) return;
    s_armed = false;
    if (s_audio->set_hw_mute) s_audio->set_hw_mute(s_audio, true);
    /* Drop the amp gate directly rather than through the route task — it may
     * not get scheduled before the caller cuts power, and the hardware write
     * is what actually needs to land. */
    if (s_audio->set_speaker_enabled) s_audio->set_speaker_enabled(s_audio, false);
}

uint32_t bsp_audio_get_caps(void) {
    return s_audio ? s_audio->caps : 0;
}

static bool pcm_available(void) {
    return s_audio && (s_audio->caps & BSP_AUDIO_CAP_PCM);
}

/* Bring a freshly (re)started stream up click-free: the DSP gain fades in
 * from silence so the clock/format transition never carries a step, the
 * codec is unmuted while the SW gain holds that silence, and — first open
 * only — the amp gate is finally armed once the DAC has settled. */
static void stream_started(uint32_t rate, uint8_t bits, uint8_t ch) {
    s_rate = rate ? rate : BSP_NOMINAL_RATE;
    if (s_dsp) {
        s_dsp_bypass = (bits != 16);
        if (!s_dsp_bypass) {
            audio_dsp_reconfig(s_dsp, s_rate, ch ? ch : 2, 16);
            apply_dsp_profile(hp_inserted_now());
            audio_dsp_set_gain(s_dsp, 0.0f, 0);
            /* Pin the codec to max while the SW gain holds silence; user
             * volume is delivered by the fade below. */
            if (s_audio->set_hw_volume) s_audio->set_hw_volume(s_audio, 100);
            audio_dsp_set_gain(s_dsp, current_target_gain(), BSP_OPEN_FADE_MS);
        }
    }
    if ((!s_dsp || s_dsp_bypass) && s_audio->set_hw_volume) {
        /* No DSP on this stream → user volume lives on the codec (0..100, no
         * amplification — the >100 boost only exists on the SW gain path). */
        int hw = s_volume < 0 ? 0 : (s_volume > 100 ? 100 : s_volume);
        s_audio->set_hw_volume(s_audio, hw);
        if (s_mute && s_audio->set_hw_mute) s_audio->set_hw_mute(s_audio, true);
    }
    if (!s_mute && s_audio->set_hw_mute) s_audio->set_hw_mute(s_audio, false);

    if (!s_armed) {
        s_armed = true;
        if (s_audio->set_speaker_enabled) {
            /* Give the analog stage a moment to settle on first power-up,
             * then let the amp follow the route policy. The amp's own startup
             * transient is unavoidable but is the only remaining click. */
            vTaskDelay(pdMS_TO_TICKS(BSP_AMP_SETTLE_MS));
            apply_speaker(s_speaker_mode);
        }
    }
}

/* Quiet the DAC before its clocks change: hw mute is the one silencing step
 * that doesn't depend on the app writing more buffers (a SW fade would). */
static void stream_stopping(void) {
    if (s_audio->set_hw_mute) {
        s_audio->set_hw_mute(s_audio, true);
        vTaskDelay(pdMS_TO_TICKS(BSP_MUTE_SETTLE_MS));
    }
}

esp_err_t bsp_audio_open(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels) {
    if (!pcm_available() || !s_audio->open) return ESP_ERR_NOT_SUPPORTED;
    if (s_open) return ESP_ERR_INVALID_STATE;
    esp_err_t err = s_audio->open(s_audio, sample_rate, bits_per_sample, channels);
    if (err != ESP_OK) return err;
    s_open = true;
    stream_started(sample_rate, bits_per_sample, channels);
    return ESP_OK;
}

esp_err_t bsp_audio_close(void) {
    if (!pcm_available() || !s_audio->close) return ESP_ERR_NOT_SUPPORTED;
    if (!s_open) return ESP_ERR_INVALID_STATE;
    stream_stopping();
    esp_err_t err = s_audio->close(s_audio);
    if (err == ESP_OK) s_open = false;
    return err;
}

esp_err_t bsp_audio_reconfig(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels) {
    if (!pcm_available() || !s_audio->reconfig) return ESP_ERR_NOT_SUPPORTED;
    if (!s_open) return ESP_ERR_INVALID_STATE;
    stream_stopping();
    esp_err_t err = s_audio->reconfig(s_audio, sample_rate, bits_per_sample, channels);
    if (err != ESP_OK) return err;
    stream_started(sample_rate, bits_per_sample, channels);
    return ESP_OK;
}

esp_err_t bsp_audio_write(void *data, size_t len) {
    if (!pcm_available() || !s_audio->write) return ESP_ERR_NOT_SUPPORTED;
    if (!s_open) return ESP_ERR_INVALID_STATE;
    if (s_dsp && !s_dsp_bypass) audio_dsp_process(s_dsp, data, len);
    return s_audio->write(s_audio, data, len);
}

esp_err_t bsp_audio_set_volume(int volume) {
    if (!pcm_available()) return ESP_ERR_NOT_SUPPORTED;
    if (volume < 0)               volume = 0;
    if (volume > BSP_VOLUME_MAX)  volume = BSP_VOLUME_MAX;
    if (volume == s_volume) return ESP_OK;  /* drop slider duplicates */
    s_volume = volume;
    if (s_dsp && !s_dsp_bypass) {
        if (s_mute || !s_open) return ESP_OK;  /* applied on unmute / open fade-in */
        return audio_dsp_set_gain(s_dsp, volume_to_gain(volume), BSP_VOLUME_FADE_MS);
    }
    if (!s_open) return ESP_OK;  /* applied by stream_started */
    /* No DSP on this stream → fall back to direct hardware volume (clicky). The
     * codec volume is 0..100 and cannot amplify, so the >100 boost is dropped. */
    int hw = volume > 100 ? 100 : volume;
    return s_audio->set_hw_volume ? s_audio->set_hw_volume(s_audio, hw)
                                  : ESP_ERR_NOT_SUPPORTED;
}

int bsp_audio_get_volume(void) {
    return s_volume < 0 ? 0 : s_volume;
}

esp_err_t bsp_audio_set_mute(bool mute) {
    if (!pcm_available()) return ESP_ERR_NOT_SUPPORTED;
    if (mute == s_mute) return ESP_OK;
    s_mute = mute;
    if (!s_open) return ESP_OK;  /* applied by stream_started */
    if (s_dsp && !s_dsp_bypass) {
        return audio_dsp_set_gain(s_dsp, current_target_gain(), BSP_VOLUME_FADE_MS);
    }
    return s_audio->set_hw_mute ? s_audio->set_hw_mute(s_audio, mute)
                                : ESP_ERR_NOT_SUPPORTED;
}

bool bsp_audio_get_mute(void) {
    return s_mute;
}

audio_dsp_t bsp_audio_dsp(void) {
    return s_dsp;
}

esp_err_t bsp_audio_set_eq_enabled(bool enabled) {
    if (!s_dsp) return ESP_ERR_NOT_SUPPORTED;  /* no DSP (DISABLE mode / no PCM) */
    /* Record the override so route re-voicing (apply_dsp_profile) keeps it, then
     * apply it now. In MANUAL mode there is no re-voicing, but the override still
     * gives a single, consistent entry point alongside bsp_audio_dsp(). */
    s_eq_override = enabled ? 1 : 0;
    return audio_dsp_set_eq_enabled(s_dsp, enabled);
}

bool bsp_audio_get_eq_enabled(void) {
    return s_dsp ? audio_dsp_is_eq_enabled(s_dsp) : false;
}

esp_err_t bsp_audio_set_speaker_mode(bsp_audio_speaker_mode_t mode) {
    if (mode != BSP_AUDIO_SPEAKER_MODE_ON && mode != BSP_AUDIO_SPEAKER_MODE_AUTO &&
        mode != BSP_AUDIO_SPEAKER_MODE_OFF) return ESP_ERR_INVALID_ARG;
    if (!s_audio || !(s_audio->caps & BSP_AUDIO_CAP_SPEAKER)) return ESP_ERR_NOT_SUPPORTED;
    if (mode == BSP_AUDIO_SPEAKER_MODE_AUTO &&
        !(s_audio->caps & BSP_AUDIO_CAP_HEADPHONE)) return ESP_ERR_NOT_SUPPORTED;
    s_speaker_mode = mode;
    if (mode == BSP_AUDIO_SPEAKER_MODE_AUTO) {
        esp_err_t err = start_route_task_once();
        if (err != ESP_OK) return err;
    }
    if (s_route_task) {
        xTaskNotifyGive(s_route_task);  /* task re-evaluates + re-arms wait */
    } else {
        apply_speaker(mode);
    }
    return ESP_OK;
}

bsp_audio_speaker_mode_t bsp_audio_get_speaker_mode(void) {
    return s_speaker_mode;
}

bool bsp_audio_headphone_inserted(void) {
    return hp_inserted_now();
}

esp_err_t bsp_audio_set_headphone_callback(bsp_audio_headphone_cb_t cb, void *user) {
    if (!s_audio || !(s_audio->caps & BSP_AUDIO_CAP_HEADPHONE)) return ESP_ERR_NOT_SUPPORTED;
    portENTER_CRITICAL(&s_hp_mux);
    s_hp_cb_arg = user;
    s_hp_cb     = cb;
    portEXIT_CRITICAL(&s_hp_mux);
    if (cb) {
        esp_err_t err = start_route_task_once();
        if (err != ESP_OK) return err;
        xTaskNotifyGive(s_route_task);  /* re-evaluate need_poll */
    }
    return ESP_OK;
}

esp_err_t bsp_audio_tone(uint32_t freq_hz, uint32_t duration_ms) {
    if (!s_audio || !(s_audio->caps & BSP_AUDIO_CAP_TONE) || !s_audio->tone) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_audio->tone(s_audio, freq_hz, duration_ms);
}

esp_err_t bsp_audio_tone_stop(void) {
    if (!s_audio || !(s_audio->caps & BSP_AUDIO_CAP_TONE) || !s_audio->tone_stop) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_audio->tone_stop(s_audio);
}
