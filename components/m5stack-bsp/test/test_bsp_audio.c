/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Host test for the bsp_audio dispatch policy, against a stub provider (no
 * SDL, no hardware): DSP voicing modes (Auto applies the board profile at
 * boot/open and re-voices on headphone insert/remove via the route task;
 * Manual stays flat; Disable has no DSP), amp arming (the speaker gate stays
 * off until the first open, then follows ON/AUTO/OFF + HP state), and the
 * headphone insert callback.
 */

#include "bsp.h"
#include "bsp_audio.h"
#include <stdio.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s — ", __FILE__, __LINE__, #cond); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

/* ---- stub provider: records every hardware op ---- */

typedef struct {
    bsp_audio_t base;
    volatile bool hp;               /* simulated jack state */
    volatile bool speaker_enabled;  /* last amp gate write */
    volatile bool hw_muted;
    volatile int  hw_volume;
    volatile bool open;
} stub_t;

static esp_err_t st_open(bsp_audio_t *self, uint32_t r, uint8_t b, uint8_t c) {
    (void)r; (void)b; (void)c;
    ((stub_t *)self)->open = true;
    return ESP_OK;
}
static esp_err_t st_close(bsp_audio_t *self) { ((stub_t *)self)->open = false; return ESP_OK; }
static esp_err_t st_reconfig(bsp_audio_t *self, uint32_t r, uint8_t b, uint8_t c) {
    (void)self; (void)r; (void)b; (void)c;
    return ESP_OK;
}
static esp_err_t st_write(bsp_audio_t *self, const void *d, size_t l) {
    (void)self; (void)d; (void)l;
    return ESP_OK;
}
static esp_err_t st_set_hw_volume(bsp_audio_t *self, int v) { ((stub_t *)self)->hw_volume = v; return ESP_OK; }
static esp_err_t st_set_hw_mute(bsp_audio_t *self, bool m) { ((stub_t *)self)->hw_muted = m; return ESP_OK; }
static esp_err_t st_set_speaker_enabled(bsp_audio_t *self, bool e) { ((stub_t *)self)->speaker_enabled = e; return ESP_OK; }
static esp_err_t st_headphone_inserted(bsp_audio_t *self, bool *out) { *out = ((stub_t *)self)->hp; return ESP_OK; }

/* Board voicing: speaker = 1 boosted stage + monomix; HP = flat stereo. */
static esp_err_t st_get_dsp_profile(bsp_audio_t *self, bool headphone, uint32_t rate,
                                    bsp_audio_dsp_profile_t *out) {
    (void)self;
    if (headphone) {
        out->num_stages = 0;
        out->eq_enabled = false;
        out->mono_mix   = false;
    } else {
        out->biquads[0] = audio_dsp_design_peaking(rate ? rate : 48000, 150.0f, 1.2f, 3.0f);
        out->num_stages = 1;
        out->eq_enabled = true;
        out->mono_mix   = true;
    }
    return ESP_OK;
}

static stub_t s_stub;

static bsp_audio_t *make_stub(void) {
    stub_t *s = &s_stub;
    *s = (stub_t){0};
    s->base.caps = BSP_AUDIO_CAP_PCM | BSP_AUDIO_CAP_SPEAKER | BSP_AUDIO_CAP_HEADPHONE;
    s->base.open                = st_open;
    s->base.close               = st_close;
    s->base.reconfig            = st_reconfig;
    s->base.write               = st_write;
    s->base.set_hw_volume       = st_set_hw_volume;
    s->base.set_hw_mute         = st_set_hw_mute;
    s->base.set_speaker_enabled = st_set_speaker_enabled;
    s->base.headphone_inserted  = st_headphone_inserted;
    s->base.get_dsp_profile     = st_get_dsp_profile;
    return &s->base;
}

/* The route task polls at 200 ms; give it time to react. */
static void route_settle(void) { usleep(600 * 1000); }

static volatile int g_cb_count;
static volatile bool g_cb_last;
static void hp_cb(bool inserted, void *user) {
    (void)user;
    g_cb_last = inserted;
    g_cb_count++;
}

static void test_auto_mode(void) {
    bsp_audio_set_active(make_stub(), NULL);   /* zero-init: Auto + speaker ON */

    audio_dsp_t dsp = bsp_audio_dsp();
    CHECK(dsp != NULL, "Auto: DSP exists from boot");
    CHECK(audio_dsp_get_mono_mix(dsp), "Auto: speaker profile applied at boot");
    CHECK(audio_dsp_is_eq_enabled(dsp), "Auto: speaker EQ on");

    /* Amp stays off until the first open, even with speaker mode ON. */
    CHECK(!s_stub.speaker_enabled, "amp off before first open");
    int16_t buf[32] = {0};
    CHECK(bsp_audio_write(buf, sizeof(buf)) == ESP_ERR_INVALID_STATE, "write before open");

    CHECK(bsp_audio_open(44100, 16, 2) == ESP_OK, "open");
    CHECK(s_stub.speaker_enabled, "amp armed by first open");
    CHECK(s_stub.hw_volume == 100, "codec pinned to max (SW gain owns volume)");
    CHECK(bsp_audio_write(buf, sizeof(buf)) == ESP_OK, "write");

    /* HP insert: Auto re-voices the DSP and (speaker AUTO) drops the amp. */
    CHECK(bsp_audio_set_speaker_mode(BSP_AUDIO_SPEAKER_MODE_AUTO) == ESP_OK, "speaker AUTO");
    CHECK(bsp_audio_set_headphone_callback(hp_cb, NULL) == ESP_OK, "register cb");
    route_settle();
    s_stub.hp = true;
    route_settle();
    CHECK(!audio_dsp_get_mono_mix(dsp), "HP in: monomix off");
    CHECK(!audio_dsp_is_eq_enabled(dsp), "HP in: speaker EQ off");
    CHECK(!s_stub.speaker_enabled, "HP in: amp off (speaker AUTO)");
    CHECK(g_cb_count == 1 && g_cb_last, "insert callback fired");

    s_stub.hp = false;
    route_settle();
    CHECK(audio_dsp_get_mono_mix(dsp), "HP out: speaker profile back");
    CHECK(s_stub.speaker_enabled, "HP out: amp back on");
    CHECK(g_cb_count == 2 && !g_cb_last, "remove callback fired");
    bsp_audio_set_headphone_callback(NULL, NULL);

    /* close keeps the amp (per route policy) but hw-mutes the DAC first. */
    CHECK(bsp_audio_close() == ESP_OK, "close");
    CHECK(s_stub.hw_muted, "close hw-muted the DAC");
    CHECK(s_stub.speaker_enabled, "close keeps the amp");

    bsp_audio_quiesce();
    CHECK(!s_stub.speaker_enabled, "quiesce drops the amp");
}

static void test_manual_mode(void) {
    bsp_audio_set_active(make_stub(), &(bsp_audio_init_t){
        .dsp_mode = BSP_AUDIO_DSP_MODE_MANUAL,
        .speaker_mode = BSP_AUDIO_SPEAKER_MODE_AUTO,
    });
    audio_dsp_t dsp = bsp_audio_dsp();
    CHECK(dsp != NULL, "Manual: DSP exists");
    CHECK(!audio_dsp_get_mono_mix(dsp), "Manual: flat init (no board profile)");
    CHECK(!audio_dsp_is_eq_enabled(dsp), "Manual: EQ off");

    CHECK(bsp_audio_open(48000, 16, 2) == ESP_OK, "open");
    audio_dsp_set_mono_mix(dsp, true);   /* app's own setting... */
    s_stub.hp = true;
    route_settle();                       /* ...survives an HP flip */
    CHECK(audio_dsp_get_mono_mix(dsp), "Manual: route change doesn't re-voice");
    CHECK(!s_stub.speaker_enabled, "speaker AUTO still routes the amp");
    bsp_audio_close();
}

static void test_disable_mode(void) {
    bsp_audio_set_active(make_stub(), &(bsp_audio_init_t){
        .dsp_mode = BSP_AUDIO_DSP_MODE_DISABLE,
    });
    CHECK(bsp_audio_dsp() == NULL, "Disable: no DSP");
    CHECK(bsp_audio_set_volume(40) == ESP_OK, "volume stored pre-open");
    CHECK(bsp_audio_open(48000, 16, 2) == ESP_OK, "open");
    CHECK(s_stub.hw_volume == 40, "no DSP: user volume lands on the codec");
    CHECK(bsp_audio_set_volume(55) == ESP_OK, "hw volume path");
    CHECK(s_stub.hw_volume == 55, "hw volume applied");
    bsp_audio_close();
}

int main(void) {
    test_auto_mode();
    test_manual_mode();
    test_disable_mode();
    bsp_audio_set_active(NULL, NULL);

    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("all bsp_audio tests passed\n");
    return 0;
}
