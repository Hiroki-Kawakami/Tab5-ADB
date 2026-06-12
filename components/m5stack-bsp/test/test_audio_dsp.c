/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Host unit test for the audio_dsp chain (EQ -> gain fade -> mono mix): pure
 * arithmetic, no audio device. Covers the fade ramp (shape, exact landing,
 * chunk-split invariance — a fade must continue seamlessly across process()
 * boundaries), per-stage bypass (bit-exactness), mono mix, EQ frequency
 * response sanity via the RBJ designers, and output clipping.
 */

#include "audio_dsp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s — ", __FILE__, __LINE__, #cond); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static audio_dsp_t make_dsp(uint32_t rate, uint8_t ch) {
    audio_dsp_config_t cfg = {
        .sample_rate = rate, .channels = ch, .bits_per_sample = 16,
    };
    audio_dsp_t dsp = NULL;
    if (audio_dsp_init(&cfg, &dsp) != ESP_OK) {
        printf("FATAL: audio_dsp_init failed\n");
        exit(1);
    }
    return dsp;
}

static void test_full_bypass_bit_exact(void) {
    audio_dsp_t dsp = make_dsp(48000, 2);
    int16_t buf[256], ref[256];
    for (int i = 0; i < 256; i++) buf[i] = ref[i] = (int16_t)(i * 257 - 32768);
    CHECK(audio_dsp_process(dsp, buf, sizeof(buf)) == ESP_OK, "process");
    CHECK(memcmp(buf, ref, sizeof(buf)) == 0, "bypassed chain must not touch the buffer");
    audio_dsp_deinit(dsp);
}

static void test_gain_fade_ramp(void) {
    /* 1 kHz sample rate, mono: 100 ms fade = exactly 100 frames. */
    audio_dsp_t dsp = make_dsp(1000, 1);
    audio_dsp_set_gain(dsp, 0.0f, 0);
    audio_dsp_set_gain(dsp, 1.0f, 100);
    CHECK(fabsf(audio_dsp_get_gain(dsp) - 1.0f) < 1e-6f, "target gain");

    enum { N = 200 };
    int16_t buf[N];
    for (int i = 0; i < N; i++) buf[i] = 10000;
    CHECK(audio_dsp_process(dsp, buf, sizeof(buf)) == ESP_OK, "process");

    for (int i = 1; i < 100; i++) {
        CHECK(buf[i] >= buf[i - 1], "ramp must be monotonic at frame %d (%d < %d)",
              i, buf[i], buf[i - 1]);
    }
    CHECK(buf[0] < 500, "fade starts near silence (got %d)", buf[0]);
    for (int i = 100; i < N; i++) {
        CHECK(buf[i] == 10000, "post-fade frame %d must be at target (got %d)", i, buf[i]);
    }
    audio_dsp_deinit(dsp);
}

static void test_gain_fade_chunk_invariance(void) {
    /* The same fade processed in one buffer vs odd-sized chunks must produce
     * identical output — the fade cursor persists across process() calls. */
    enum { N = 750 };
    int16_t one[N], chunked[N];
    for (int i = 0; i < N; i++) one[i] = chunked[i] = 12345;

    audio_dsp_t a = make_dsp(1000, 1);
    audio_dsp_set_gain(a, 0.0f, 0);
    audio_dsp_set_gain(a, 0.8f, 500);
    audio_dsp_process(a, one, sizeof(one));
    audio_dsp_deinit(a);

    audio_dsp_t b = make_dsp(1000, 1);
    audio_dsp_set_gain(b, 0.0f, 0);
    audio_dsp_set_gain(b, 0.8f, 500);
    size_t off = 0;
    const size_t chunks[] = { 13, 1, 257, 64, 199, 216 };  /* sums to 750 */
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        audio_dsp_process(b, chunked + off, chunks[c] * sizeof(int16_t));
        off += chunks[c];
    }
    CHECK(off == N, "chunk sizes cover the buffer");
    audio_dsp_deinit(b);

    CHECK(memcmp(one, chunked, sizeof(one)) == 0,
          "chunked fade must match single-buffer fade");
}

static void test_gain_clamp(void) {
    audio_dsp_t dsp = make_dsp(48000, 1);
    audio_dsp_set_gain(dsp, 2.0f, 0);  /* >1.0 amplifies (boost) */
    CHECK(fabsf(audio_dsp_get_gain(dsp) - 2.0f) < 1e-6f, "gain >1.0 is accepted");
    audio_dsp_set_gain(dsp, 100.0f, 0);
    CHECK(fabsf(audio_dsp_get_gain(dsp) - 4.0f) < 1e-6f, "gain clamps to +12 dB ceiling");
    audio_dsp_set_gain(dsp, -1.0f, 0);
    CHECK(audio_dsp_get_gain(dsp) == 0.0f, "gain clamps to 0.0");
    audio_dsp_deinit(dsp);
}

static void test_gain_boost(void) {
    /* gain > 1.0 amplifies; the output saturates to int16 instead of wrapping. */
    audio_dsp_t dsp = make_dsp(48000, 1);
    audio_dsp_set_gain(dsp, 2.0f, 0);  /* +6 dB, no fade */
    int16_t buf[4] = {1000, -1000, 30000, -30000};
    CHECK(audio_dsp_process(dsp, buf, sizeof(buf)) == ESP_OK, "process");
    CHECK(buf[0] == 2000 && buf[1] == -2000, "in-range samples doubled");
    CHECK(buf[2] == 32767, "over-range positive saturates (not wrap)");
    CHECK(buf[3] == -32768, "over-range negative saturates (not wrap)");
    audio_dsp_deinit(dsp);
}

static void test_mono_mix(void) {
    audio_dsp_t dsp = make_dsp(48000, 2);
    audio_dsp_set_mono_mix(dsp, true);
    CHECK(audio_dsp_get_mono_mix(dsp), "mono mix enabled");

    int16_t buf[8] = { 1000, -1000, 2000, 0, -500, -500, 32767, 32767 };
    audio_dsp_process(dsp, buf, sizeof(buf));
    CHECK(buf[0] == 0 && buf[1] == 0, "L+R cancel (got %d,%d)", buf[0], buf[1]);
    CHECK(buf[2] == 1000 && buf[3] == 1000, "half sum (got %d,%d)", buf[2], buf[3]);
    CHECK(buf[4] == -500 && buf[5] == -500, "equal channels unchanged");
    CHECK(buf[6] == 32767 && buf[7] == 32767, "full scale survives");

    audio_dsp_set_mono_mix(dsp, false);
    int16_t buf2[2] = { 1000, -1000 };
    audio_dsp_process(dsp, buf2, sizeof(buf2));
    CHECK(buf2[0] == 1000 && buf2[1] == -1000, "disabled mix leaves channels");
    audio_dsp_deinit(dsp);
}

static void test_eq_lowpass_response(void) {
    const uint32_t fs = 48000;
    audio_dsp_t dsp = make_dsp(fs, 1);
    audio_dsp_biquad_t lp = audio_dsp_design_lowpass(fs, 1000.0f, 0.707f);
    audio_dsp_set_biquads(dsp, &lp, 1);
    audio_dsp_set_eq_enabled(dsp, true);
    CHECK(audio_dsp_is_eq_enabled(dsp), "eq enabled");

    /* DC passes ~unity. */
    enum { N = 2000 };
    static int16_t buf[N];
    for (int i = 0; i < N; i++) buf[i] = 8000;
    audio_dsp_process(dsp, buf, sizeof(buf));
    CHECK(abs(buf[N - 1] - 8000) < 80, "DC gain ~1 (got %d)", buf[N - 1]);

    /* Nyquist-rate alternation is strongly attenuated. */
    audio_dsp_set_biquads(dsp, &lp, 1);  /* reset filter state */
    for (int i = 0; i < N; i++) buf[i] = (i & 1) ? -8000 : 8000;
    audio_dsp_process(dsp, buf, sizeof(buf));
    int peak = 0;
    for (int i = N / 2; i < N; i++) {
        int v = abs(buf[i]);
        if (v > peak) peak = v;
    }
    CHECK(peak < 100, "Nyquist attenuated by 1 kHz lowpass (peak %d)", peak);

    /* Disabled EQ is a true bypass. */
    audio_dsp_set_eq_enabled(dsp, false);
    int16_t raw[4] = { 8000, -8000, 8000, -8000 };
    audio_dsp_process(dsp, raw, sizeof(raw));
    CHECK(raw[0] == 8000 && raw[1] == -8000, "disabled eq leaves signal");
    audio_dsp_deinit(dsp);
}

static void test_clipping(void) {
    /* A +12 dB low shelf on a near-full-scale signal must clamp, not wrap. */
    const uint32_t fs = 48000;
    audio_dsp_t dsp = make_dsp(fs, 1);
    audio_dsp_biquad_t shelf = audio_dsp_design_low_shelf(fs, 10000.0f, 0.707f, 12.0f);
    audio_dsp_set_biquads(dsp, &shelf, 1);
    audio_dsp_set_eq_enabled(dsp, true);

    enum { N = 512 };
    int16_t buf[N];
    for (int i = 0; i < N; i++) buf[i] = (i & 1) ? -30000 : 30000;
    /* Low-frequency-ish content boosted: use a slow square instead. */
    for (int i = 0; i < N; i++) buf[i] = (i / 64 % 2) ? -30000 : 30000;
    audio_dsp_process(dsp, buf, sizeof(buf));
    int boosted = 0;
    for (int i = 0; i < N; i++) {
        CHECK(buf[i] <= 32767 && buf[i] >= -32768, "sample %d in range (got %d)", i, buf[i]);
        if (abs(buf[i]) > 30000) boosted = 1;
    }
    CHECK(boosted, "shelf actually boosted the signal");
    audio_dsp_deinit(dsp);
}

static void test_stage_capacity_grows(void) {
    /* Default capacity is 5; set_biquads beyond it must grow, not fail. */
    const uint32_t fs = 48000;
    audio_dsp_t dsp = make_dsp(fs, 2);
    audio_dsp_biquad_t stages[7];
    for (int i = 0; i < 7; i++) {
        stages[i] = audio_dsp_design_peaking(fs, 100.0f * (i + 1), 1.0f, 0.0f);
    }
    CHECK(audio_dsp_set_biquads(dsp, stages, 7) == ESP_OK, "grow to 7 stages");
    audio_dsp_set_eq_enabled(dsp, true);
    int16_t buf[64];
    for (int i = 0; i < 64; i++) buf[i] = 4000;
    CHECK(audio_dsp_process(dsp, buf, sizeof(buf)) == ESP_OK, "process with 7 stages");
    /* 0 dB peaking stages are identity — DC passes through. */
    CHECK(abs(buf[62] - 4000) < 40, "identity stages pass DC (got %d)", buf[62]);
    /* Shrinking back stays fine. */
    CHECK(audio_dsp_set_biquads(dsp, stages, 2) == ESP_OK, "shrink to 2 stages");
    CHECK(audio_dsp_process(dsp, buf, sizeof(buf)) == ESP_OK, "process after shrink");
    audio_dsp_deinit(dsp);
}

static void test_reconfig(void) {
    audio_dsp_t dsp = make_dsp(48000, 2);
    CHECK(audio_dsp_reconfig(dsp, 44100, 1, 16) == ESP_OK, "reconfig 44.1k mono");
    CHECK(audio_dsp_reconfig(dsp, 44100, 1, 24) == ESP_ERR_INVALID_ARG, "24-bit rejected");
    /* Mono buffer processes after channel change. */
    audio_dsp_set_gain(dsp, 0.5f, 0);
    int16_t buf[4] = { 1000, 1000, 1000, 1000 };
    CHECK(audio_dsp_process(dsp, buf, sizeof(buf)) == ESP_OK, "process after reconfig");
    CHECK(buf[0] == 500, "gain applies after reconfig (got %d)", buf[0]);
    audio_dsp_deinit(dsp);
}

int main(void) {
    test_full_bypass_bit_exact();
    test_gain_fade_ramp();
    test_gain_fade_chunk_invariance();
    test_gain_clamp();
    test_gain_boost();
    test_mono_mix();
    test_eq_lowpass_response();
    test_clipping();
    test_stage_capacity_grows();
    test_reconfig();

    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("all audio_dsp tests passed\n");
    return 0;
}
