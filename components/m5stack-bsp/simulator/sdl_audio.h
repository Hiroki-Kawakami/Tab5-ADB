/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Reusable SDL host audio backend for the BSP simulator boards: a bsp_audio
 * provider (caps PCM | SPEAKER) over SDL's queue-audio API, so app audio code
 * runs — audibly — on the desktop. write() applies backpressure once ~100 ms
 * of audio is queued, mimicking the blocking I2S DMA write on device, which is
 * what paces a real-time producer (e.g. the mirror's sound passthrough).
 *
 * When SIMULATOR_HEADLESS is set, or when no host audio device can be opened,
 * the provider falls back to a silent null sink that keeps the same real-time
 * pacing — so headless verify runs exercise the timing without a sound card.
 */

#pragma once
#include "bsp_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The provider registers closed; the host audio device is opened by the first
 * public bsp_audio_open() (which carries the stream format). */
esp_err_t sdl_audio_create(bsp_audio_t **out_audio);

#ifdef __cplusplus
}
#endif
