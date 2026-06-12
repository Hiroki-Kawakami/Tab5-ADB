/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "sdl_audio.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "SDL_AUDIO";

/* Backpressure high-water mark: block write() once this much audio is queued.
 * Mimics the device's I2S DMA depth; keeps producer pacing real-time while
 * bounding latency. */
#define SDL_AUDIO_HIGH_WATER_MS 100

typedef struct {
    bsp_audio_t base;
    SDL_AudioDeviceID dev;      /* 0 = null sink (headless / no audio device) */
    bool stream_open;
    uint32_t rate;
    uint8_t  channels;
    size_t   frame_bytes;       /* channels * 2 (S16) */
    size_t   high_water;        /* bytes */
    /* "Hardware" silencing flags (codec mute / amp gate): audio keeps being
     * consumed at the same rate, just as zeros — pausing the device instead
     * would stall write()'s backpressure, which a real amp gate never does. */
    bool hw_mute;
    bool speaker_off;
    uint8_t *zero_buf;
    size_t   zero_cap;
    /* Null-sink pacing clock: virtual playback position vs wall clock. */
    uint64_t ns_start_ms;
    uint64_t ns_queued_us;
} sdl_audio_state_t;

static esp_err_t sa_close(bsp_audio_t *self);

static esp_err_t sa_open(bsp_audio_t *self, uint32_t rate, uint8_t bits, uint8_t ch) {
    sdl_audio_state_t *s = (sdl_audio_state_t *)self;
    if (!rate) rate = 48000;
    if (!bits) bits = 16;
    if (!ch)   ch   = 2;
    if (bits != 16) return ESP_ERR_NOT_SUPPORTED;
    if (ch < 1 || ch > 2) return ESP_ERR_INVALID_ARG;

    if (s->stream_open) sa_close(self);

    s->rate        = rate;
    s->channels    = ch;
    s->frame_bytes = (size_t)ch * 2;
    s->high_water  = (size_t)rate * s->frame_bytes * SDL_AUDIO_HIGH_WATER_MS / 1000;

    if (!getenv("SIMULATOR_HEADLESS")) {
        SDL_AudioSpec want = {0}, have;
        want.freq     = (int)rate;
        want.format   = AUDIO_S16SYS;
        want.channels = ch;
        want.samples  = 1024;
        s->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (s->dev) {
            SDL_PauseAudioDevice(s->dev, 0);
        } else {
            ESP_LOGW(TAG, "SDL_OpenAudioDevice failed (%s) — silent null sink", SDL_GetError());
        }
    }
    s->ns_start_ms  = SDL_GetTicks64();
    s->ns_queued_us = 0;
    s->stream_open  = true;
    return ESP_OK;
}

static esp_err_t sa_close(bsp_audio_t *self) {
    sdl_audio_state_t *s = (sdl_audio_state_t *)self;
    if (!s->stream_open) return ESP_ERR_INVALID_STATE;
    if (s->dev) {
        SDL_ClearQueuedAudio(s->dev);
        SDL_CloseAudioDevice(s->dev);
        s->dev = 0;
    }
    s->stream_open = false;
    return ESP_OK;
}

static esp_err_t sa_reconfig(bsp_audio_t *self, uint32_t rate, uint8_t bits, uint8_t ch) {
    return sa_open(self, rate, bits, ch);
}

static esp_err_t sa_write(bsp_audio_t *self, const void *data, size_t len) {
    sdl_audio_state_t *s = (sdl_audio_state_t *)self;
    if (!s->stream_open) return ESP_ERR_INVALID_STATE;

    const void *out = data;
    if (s->hw_mute || s->speaker_off) {
        if (s->zero_cap < len) {
            uint8_t *grown = realloc(s->zero_buf, len);
            if (!grown) return ESP_ERR_NO_MEM;
            memset(grown, 0, len);
            s->zero_buf = grown;
            s->zero_cap = len;
        }
        out = s->zero_buf;
    }

    if (s->dev) {
        while (SDL_GetQueuedAudioSize(s->dev) + len > s->high_water) {
            SDL_Delay(2);
        }
        return SDL_QueueAudio(s->dev, out, (Uint32)len) == 0 ? ESP_OK : ESP_FAIL;
    }

    /* Null sink: advance a virtual playback clock and sleep so the producer
     * sees the same real-time backpressure as a sound device. */
    s->ns_queued_us += (uint64_t)len * 1000000u / ((uint64_t)s->rate * s->frame_bytes);
    uint64_t elapsed_us = (SDL_GetTicks64() - s->ns_start_ms) * 1000u;
    uint64_t high_water_us = (uint64_t)SDL_AUDIO_HIGH_WATER_MS * 1000u;
    if (s->ns_queued_us > elapsed_us + high_water_us) {
        SDL_Delay((Uint32)((s->ns_queued_us - elapsed_us - high_water_us) / 1000u));
    }
    return ESP_OK;
}

static esp_err_t sa_set_hw_mute(bsp_audio_t *self, bool mute) {
    ((sdl_audio_state_t *)self)->hw_mute = mute;
    return ESP_OK;
}

static esp_err_t sa_set_speaker_enabled(bsp_audio_t *self, bool enabled) {
    ((sdl_audio_state_t *)self)->speaker_off = !enabled;
    return ESP_OK;
}

static esp_err_t sa_deinit(bsp_audio_t *self) {
    sdl_audio_state_t *s = (sdl_audio_state_t *)self;
    if (s->stream_open) sa_close(self);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    free(s->zero_buf);
    free(s);
    return ESP_OK;
}

esp_err_t sdl_audio_create(bsp_audio_t **out_audio) {
    if (!out_audio) return ESP_ERR_INVALID_ARG;

    /* Audio is its own SDL subsystem — independent of the video/timer init the
     * display backend does, and refcounted, so this is safe either way. */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        ESP_LOGW(TAG, "SDL_InitSubSystem(AUDIO) failed (%s) — null sink only", SDL_GetError());
    }

    sdl_audio_state_t *s = calloc(1, sizeof(*s));
    if (!s) return ESP_ERR_NO_MEM;

    s->base.caps = BSP_AUDIO_CAP_PCM | BSP_AUDIO_CAP_SPEAKER;
    s->base.open                = sa_open;
    s->base.close               = sa_close;
    s->base.reconfig            = sa_reconfig;
    s->base.write               = sa_write;
    s->base.set_hw_mute         = sa_set_hw_mute;
    s->base.set_speaker_enabled = sa_set_speaker_enabled;
    s->base.deinit              = sa_deinit;

    /* Registers closed — the host device opens on the first public open(). */
    *out_audio = &s->base;
    return ESP_OK;
}
