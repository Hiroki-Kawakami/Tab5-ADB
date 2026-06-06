/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Model-agnostic public BSP API. bsp_init() dispatches to the board selected at
 * build time (see boards/<model>/); within a board the panel generation is
 * resolved by the board itself. Board-specific surfaces that have no generic
 * contract yet (e.g. audio) live in their own headers (bsp_audio.h).
 */

#pragma once
#include "bsp_types.h"
#include "audio_eq.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BSP_WIFI_MODE_NONE = 0,
    BSP_WIFI_MODE_STA  = 1 << 0,
    BSP_WIFI_MODE_AP   = 1 << 1,
} bsp_wifi_mode_t;

typedef enum {
    BSP_SPEAKER_MODE_ON   = 0,  /*!< amp always on (default — matches zero-init) */
    BSP_SPEAKER_MODE_AUTO = 1,  /*!< amp on only while HP jack is unplugged */
    BSP_SPEAKER_MODE_OFF  = 2,  /*!< amp always off */
} bsp_speaker_mode_t;

typedef struct {
    struct {
        uint8_t fb_num;
        bsp_pixel_format_t pixel_format;
    } display;
    struct {
        bool interrupt;
    } touch;
    struct {
        bool usb5v_en;
    } usb;
    struct {
        bsp_wifi_mode_t mode;
    } wifi;
    struct {
        bool enable;
    } bluetooth;
    struct {
        bool disable;            /*!< Skip audio codec init (default: enabled) */
        uint32_t sample_rate;    /*!< 0 -> 48000 */
        uint8_t bits_per_sample; /*!< 0 -> 16 */
        uint8_t channels;        /*!< 0 -> 2 */
        struct {
            bool enable;                          /*!< Enable EQ at boot */
            size_t num_stages;                    /*!< Number of biquads in `biquads` */
            const audio_eq_biquad_t *biquads;     /*!< Initial coefficients (copied) */
            size_t max_stages;                    /*!< Capacity; 0 -> max(num_stages, 8) */
        } eq;
        bsp_speaker_mode_t speaker_mode;          /*!< Speaker amp policy at boot */
    } audio;
} bsp_config_t;

esp_err_t bsp_init(const bsp_config_t *config);
void bsp_restart(void);

// MARK: Display
void  bsp_display_set_brightness(int brightness);
void *bsp_display_get_frame_buffer(int fb_index);
void  bsp_display_flush(int fb_index);

// MARK: Touch
int   bsp_touch_read(bsp_touch_point_t *points, uint8_t max_points);
void  bsp_touch_wait_interrupt(void);

#ifdef __cplusplus
}
#endif
