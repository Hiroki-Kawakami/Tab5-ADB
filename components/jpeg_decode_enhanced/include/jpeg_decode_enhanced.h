#pragma once

/*
 * jpeg_decode_enhanced — Layer 1: strip-pipelined JPEG hardware decode.
 *
 * Replays the bulk of ESP-IDF's jpeg_decoder_process() flow but writes the
 * decoded raster into a ring of strip buffers via linked 2D-DMA RX
 * descriptors. Each finished strip fires on_strip_done() from ISR context so
 * a downstream consumer (PPA, CPU, ...) can start processing it immediately,
 * in parallel with the decode of subsequent strips. Backpressure works by
 * splicing descriptors back into the chain only after the consumer releases
 * the corresponding ring slot (jpeg_enh_strip_decoder_release_strip).
 *
 * Also provides a whole-frame convenience API (jpeg_enh_decoder_process)
 * that decodes into a single caller-supplied buffer — useful when only the
 * enhanced color conversion (e.g. full-range YUV->RGB) is wanted and no strip
 * pipelining is needed.
 *
 * Constraints that remain (hardware):
 *  - Strip boundaries must lie on JPEG MCU-row boundaries. The 2D-DMA RX
 *    reorder unit works on macro blocks matching the JPEG sampling (8 rows
 *    for YUV444/YUV422/GRAY, 16 rows for YUV420), and one MCU row cannot
 *    span two descriptors/buffers. strip_h_hint is therefore rounded to a
 *    multiple of the frame's MCU height at process time.
 *  - The decoded raster width is the MCU-padded width (frame_info.pic_w);
 *    rows are stored contiguously at that width (no stride control).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/jpeg_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Pixel-format / color-conversion configuration shared by both the
 *        strip and whole-frame decode paths.
 */
typedef struct {
    jpeg_dec_output_format_t output_format;   /*!< Decoded pixel format (RGB565/RGB888/GRAY/YUV...) */
    jpeg_dec_rgb_element_order_t rgb_order;   /*!< RGB element order for RGB output formats */
    jpeg_yuv_rgb_conv_std_t conv_std;         /*!< BT601 or BT709 YUV->RGB conversion standard */
    bool yuv_full_range;                      /*!< false: IDF stock limited-range (BT.601/709 studio swing)
                                                   matrix. true: full-range (JFIF) matrix — what virtually
                                                   all JPEG/MJPEG content actually uses. Only affects RGB
                                                   output formats. */
} jpeg_enh_decode_cfg_t;

/**
 * @brief Per-frame geometry, filled from the parsed JPEG header.
 */
typedef struct {
    uint32_t pic_w;        /*!< MCU-padded processing width (decoded raster width) */
    uint32_t pic_h;        /*!< MCU-padded processing height (decoded raster height) */
    uint32_t origin_w;     /*!< Real image width from the SOF header */
    uint32_t origin_h;     /*!< Real image height from the SOF header */
    uint8_t mcu_w;         /*!< MCU width in pixels (8 or 16) */
    uint8_t mcu_h;         /*!< MCU height in pixels (8 or 16) */
    uint32_t strip_h;      /*!< Effective strip height chosen for this frame (multiple of mcu_h) */
    uint32_t strip_count;  /*!< Number of strips this frame (last one may be shorter) */
} jpeg_enh_frame_info_t;

/**
 * @brief Per-strip event payload delivered to on_strip_done.
 */
typedef struct {
    uint32_t strip_idx;    /*!< 0 .. strip_count-1 */
    void *buffer;          /*!< Ring buffer now holding the decoded strip */
    uint32_t y_offset;     /*!< First row of this strip in decoded-raster coordinates */
    uint32_t rows;         /*!< Valid image rows in this strip (clipped to origin_h) */
    uint32_t padded_rows;  /*!< Rows the decoder actually wrote (MCU aligned, >= rows) */
} jpeg_enh_strip_event_t;

/**
 * @brief Per-strip callback. Fired from 2D-DMA ISR context when a strip worth
 *        of decoded pixels has finished landing in its ring buffer.
 *
 * @return Whether the callback woke a higher-priority task (task yield request).
 */
typedef bool (*jpeg_enh_strip_done_cb_t)(const jpeg_enh_strip_event_t *evt, void *user_ctx);

/**
 * @brief Frame-start callback. Fired in the caller's task context from inside
 *        jpeg_enh_strip_decoder_process(), after the header has been parsed and
 *        validated but before any DMA starts. Lets the consumer set up per-frame
 *        state (strip placement etc.) before strips begin to arrive.
 *
 * @return ESP_OK to proceed; any other value aborts the frame and is returned
 *         from jpeg_enh_strip_decoder_process().
 */
typedef esp_err_t (*jpeg_enh_frame_start_cb_t)(const jpeg_enh_frame_info_t *info, void *user_ctx);

/**
 * @brief Strip decoder configuration.
 */
typedef struct {
    jpeg_enh_decode_cfg_t decode;     /*!< Pixel format / color conversion */

    uint32_t max_pic_w;               /*!< Largest decoded-raster width to support (sizes the strip buffers) */
    uint32_t max_pic_h;               /*!< Largest decoded-raster height to support (sizes the descriptor chain) */
    uint32_t strip_h_hint;            /*!< Desired strip height in rows; rounded down to the frame's MCU height
                                           at process time (minimum one MCU row). 0 = single strip covering the
                                           whole image. */
    uint32_t ring_count;              /*!< Number of strip ring buffers. 0 = no strip mode (whole-frame API only). */
    uint32_t strip_alloc_caps;        /*!< heap_caps for the internally-allocated strip buffers.
                                           0 = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL (fast path).
                                           Pass MALLOC_CAP_SPIRAM to keep the ring in PSRAM — slower, but frees
                                           internal SRAM; cache maintenance is handled internally (see
                                           jpeg_enh_strip_decoder_sync_strip_for_cpu for CPU consumers). */

    int intr_priority;                /*!< JPEG engine interrupt priority (0 = default) */
    uint32_t timeout_ms;              /*!< Decode timeout (0 = 200 ms) */

    jpeg_enh_frame_start_cb_t on_frame_start;  /*!< Optional; caller task context */
    jpeg_enh_strip_done_cb_t on_strip_done;    /*!< Required for strip mode; ISR context */
    void *user_ctx;                   /*!< Passed to both callbacks */
} jpeg_enh_strip_decoder_cfg_t;

typedef struct jpeg_enh_strip_decoder_s *jpeg_enh_strip_decoder_handle_t;

/**
 * @brief Acquire the JPEG hardware and allocate strip-mode resources.
 *
 * Internally grabs the IDF JPEG decoder engine (DMA pool, codec mutex, ISR)
 * and allocates the descriptor chain plus, when ring_count > 0, the strip
 * ring buffers.
 */
esp_err_t jpeg_enh_strip_decoder_new(const jpeg_enh_strip_decoder_cfg_t *cfg,
                                     jpeg_enh_strip_decoder_handle_t *out_handle);

/**
 * @brief Release the decoder and all internally-allocated buffers.
 */
esp_err_t jpeg_enh_strip_decoder_del(jpeg_enh_strip_decoder_handle_t handle);

/**
 * @brief Decode one JPEG image, strip by strip.
 *
 * Blocks until the JPEG hardware signals end-of-frame. While running,
 * on_strip_done fires from ISR context for each strip. The consumer must call
 * jpeg_enh_strip_decoder_release_strip() for every strip once done with its
 * buffer, or the chained DMA stalls and the frame fails with ESP_ERR_TIMEOUT
 * (logged as a ring underrun).
 *
 * The image may be any size up to (max_pic_w, max_pic_h); geometry is
 * re-derived from the header every frame.
 *
 * @param info Optional; receives this frame's geometry.
 */
esp_err_t jpeg_enh_strip_decoder_process(jpeg_enh_strip_decoder_handle_t handle,
                                         const uint8_t *bit_stream, uint32_t stream_size,
                                         jpeg_enh_frame_info_t *info);

/**
 * @brief Tell the decoder that the consumer is done with strip_idx, so its
 *        ring slot can be reused for a later strip of the same frame.
 *
 * Safe from any task context.
 */
esp_err_t jpeg_enh_strip_decoder_release_strip(jpeg_enh_strip_decoder_handle_t handle,
                                               uint32_t strip_idx);

/**
 * @brief Prepare a strip buffer for CPU reads.
 *
 * When the strip ring lives in PSRAM the CPU data cache may hold stale lines
 * for the buffer region; this invalidates them. No-op for internal-RAM rings
 * (not cached on ESP32-P4) and for pure DMA consumers like PPA.
 * Call it from task context before CPU-reading a strip, never from ISR.
 */
esp_err_t jpeg_enh_strip_decoder_sync_strip_for_cpu(jpeg_enh_strip_decoder_handle_t handle,
                                                    const jpeg_enh_strip_event_t *evt);

/**
 * @brief Number of strips delivered (on_strip_done fired) for the current /
 *        most recent frame. Useful for consumer-side cleanup after an error.
 */
uint32_t jpeg_enh_strip_decoder_strips_delivered(jpeg_enh_strip_decoder_handle_t handle);

/**
 * @brief Byte size of each internally-allocated strip ring buffer.
 */
size_t jpeg_enh_strip_decoder_strip_buffer_size(jpeg_enh_strip_decoder_handle_t handle);

/**
 * @brief Whole-frame convenience decode: parse + decode one JPEG into a single
 *        caller-supplied buffer, with the same enhanced color conversion
 *        (full-range matrices) as the strip path. No strip buffers, callbacks
 *        or release calls are involved.
 *
 * The output buffer must satisfy the same alignment rules as IDF's
 * jpeg_decoder_process(): allocate it with jpeg_alloc_decoder_mem()
 * (JPEG_DEC_ALLOC_OUTPUT_BUFFER) or equivalent cache-line-aligned allocation.
 * Required capacity is pic_w * pic_h * bits_per_pixel / 8 for the MCU-padded
 * size, rounded up to the cache line for PSRAM buffers.
 *
 * Works on any handle, including ring_count = 0 ones.
 */
esp_err_t jpeg_enh_decoder_process(jpeg_enh_strip_decoder_handle_t handle,
                                   const uint8_t *bit_stream, uint32_t stream_size,
                                   void *out_buf, size_t out_buf_size,
                                   jpeg_enh_frame_info_t *info);

#ifdef __cplusplus
}
#endif
