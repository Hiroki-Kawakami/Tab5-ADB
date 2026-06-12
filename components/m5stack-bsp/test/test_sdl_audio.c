/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Host test for the bsp_audio dispatch + SDL provider: plays a sine through
 * the full public path (set_active -> set_volume -> open -> write with the
 * DSP gain fade) and asserts the write() backpressure paces the producer in
 * real time. With a sound device you should HEAR ~2 s of 440 Hz fading in,
 * dipping to silence (mute toggle), and returning; without one (or with
 * SIMULATOR_HEADLESS=1) the null sink keeps the same timing silently.
 */

#include "bsp.h"
#include "bsp_audio.h"
#include "sdl_audio.h"
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <SDL2/SDL.h>

static int g_failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s — ", __FILE__, __LINE__, #cond); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

#define RATE 48000
#define CH   2
#define CHUNK_FRAMES 960   /* 20 ms */

static void write_sine_ms(uint32_t ms, double *phase) {
    static int16_t buf[CHUNK_FRAMES * CH];
    const double step = 2.0 * M_PI * 440.0 / RATE;
    for (uint32_t done = 0; done < ms; done += 20) {
        for (int f = 0; f < CHUNK_FRAMES; f++) {
            int16_t s = (int16_t)(20000.0 * sin(*phase));
            *phase += step;
            buf[f * CH]     = s;
            buf[f * CH + 1] = s;
        }
        CHECK(bsp_audio_write(buf, sizeof(buf)) == ESP_OK, "write");
    }
}

int main(void) {
    bsp_audio_t *audio = NULL;
    esp_err_t err = sdl_audio_create(&audio);
    CHECK(err == ESP_OK, "sdl_audio_create: %d", err);
    if (err != ESP_OK) return 1;

    bsp_audio_set_active(audio, NULL);   /* zero-init = DSP Auto, speaker ON */

    CHECK(bsp_audio_get_caps() == (BSP_AUDIO_CAP_PCM | BSP_AUDIO_CAP_SPEAKER), "caps");
    CHECK(bsp_audio_dsp() != NULL, "dispatch owns a DSP instance from boot");
    CHECK(bsp_audio_set_speaker_mode(BSP_AUDIO_SPEAKER_MODE_AUTO) == ESP_ERR_NOT_SUPPORTED,
          "AUTO needs CAP_HEADPHONE");
    CHECK(!bsp_audio_headphone_inserted(), "no HP detect");
    CHECK(bsp_audio_tone(440, 100) == ESP_ERR_NOT_SUPPORTED, "no buzzer");
    CHECK(bsp_audio_get_volume() == 0, "volume starts at 0");

    /* No signal path before open. */
    int16_t dummy[CH] = {0};
    CHECK(bsp_audio_write(dummy, sizeof(dummy)) == ESP_ERR_INVALID_STATE, "write before open");
    CHECK(bsp_audio_close() == ESP_ERR_INVALID_STATE, "close before open");
    CHECK(bsp_audio_reconfig(RATE, 16, CH) == ESP_ERR_INVALID_STATE, "reconfig before open");
    /* Volume set pre-open is stored and delivered by the open fade-in. */
    CHECK(bsp_audio_set_volume(70) == ESP_OK, "set_volume before open");
    CHECK(bsp_audio_get_volume() == 70, "stored volume");

    CHECK(bsp_audio_open(RATE, 16, CH) == ESP_OK, "open");
    CHECK(bsp_audio_open(RATE, 16, CH) == ESP_ERR_INVALID_STATE, "double open");

    double phase = 0.0;

    /* 2 s of audio must take ~2 s of wall time (minus the ~100 ms queue) —
     * this is the backpressure the mirror's sound passthrough will pace on. */
    uint64_t t0 = SDL_GetTicks64();
    write_sine_ms(1000, &phase);
    bsp_audio_set_mute(true);
    CHECK(bsp_audio_get_mute(), "muted");
    write_sine_ms(500, &phase);   /* audibly silent: SW gain faded to 0 */
    bsp_audio_set_mute(false);
    write_sine_ms(500, &phase);
    uint64_t elapsed = SDL_GetTicks64() - t0;
    CHECK(elapsed > 1700, "2 s of audio paced in real time (took %llu ms)",
          (unsigned long long)elapsed);
    CHECK(elapsed < 4000, "backpressure didn't over-block (took %llu ms)",
          (unsigned long long)elapsed);

    CHECK(bsp_audio_reconfig(44100, 16, 1) == ESP_OK, "reconfig");
    int16_t mono[441];
    for (int i = 0; i < 441; i++) mono[i] = (int16_t)(10000.0 * sin(phase += 0.0628));
    CHECK(bsp_audio_write(mono, sizeof(mono)) == ESP_OK, "mono write");

    CHECK(bsp_audio_close() == ESP_OK, "close");
    CHECK(bsp_audio_write(mono, sizeof(mono)) == ESP_ERR_INVALID_STATE, "write after close");
    CHECK(bsp_audio_open(RATE, 16, CH) == ESP_OK, "re-open");
    write_sine_ms(100, &phase);
    CHECK(bsp_audio_close() == ESP_OK, "re-close");

    /* DSP modes: Disable drops the chain entirely. */
    bsp_audio_set_active(audio, &(bsp_audio_init_t){ .dsp_mode = BSP_AUDIO_DSP_MODE_DISABLE });
    CHECK(bsp_audio_dsp() == NULL, "DISABLE: no DSP");
    bsp_audio_set_active(audio, &(bsp_audio_init_t){ .dsp_mode = BSP_AUDIO_DSP_MODE_MANUAL });
    CHECK(bsp_audio_dsp() != NULL, "MANUAL: DSP exists");
    CHECK(!audio_dsp_is_eq_enabled(bsp_audio_dsp()), "MANUAL: flat init");

    bsp_audio_set_active(NULL, NULL);
    CHECK(bsp_audio_get_caps() == 0, "deactivated");
    CHECK(bsp_audio_write(mono, sizeof(mono)) == ESP_ERR_NOT_SUPPORTED, "no provider");
    audio->deinit(audio);

    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("all sdl_audio tests passed\n");
    return 0;
}
