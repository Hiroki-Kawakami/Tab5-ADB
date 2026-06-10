#pragma once

/*
 * jpeg_decode_enhanced — Layer 2: strip-pipelined JPEG decode + PPA SRM.
 *
 * Decodes a JPEG into a ring of strip buffers (Layer 1) and pushes each strip
 * through PPA SRM into a destination frame buffer as soon as it lands, in
 * parallel with the decode of subsequent strips. Resources (ring buffers,
 * JPEG engine, PPA client, worker task) are fixed at creation; everything
 * about the transform — rotation, scale, mirror, crop, output placement —
 * can change per frame.
 *
 * Geometry note: each strip goes through PPA as an independent block, so
 * every interior strip boundary must scale to a whole output pixel row or
 * the strips would overlap/gap. PPA quantizes scale factors to multiples of
 * 1/16 (and 1/8 for YUV420 output); jpeg_ppa_pipeline_process() validates
 * "(boundary row × quantized scale_y) is an integer" per frame and rejects
 * the transform otherwise. With the default strip_h_hint of 16 this is
 * satisfied for every scale factor.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/ppa.h"
#include "jpeg_decode_enhanced.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jpeg_ppa_pipeline_s *jpeg_ppa_pipeline_handle_t;

/**
 * @brief Pipeline resources configuration (fixed at creation).
 */
typedef struct {
    uint32_t max_pic_w;                   /*!< Largest input width to support */
    uint32_t max_pic_h;                   /*!< Largest input height to support */
    uint32_t strip_h_hint;                /*!< Strip height hint; see jpeg_enh_strip_decoder_cfg_t.
                                               0 is not useful here (single strip = no pipelining). */
    uint32_t ring_count;                  /*!< Strip ring depth; >= 2 for any pipelining benefit */
    ppa_srm_color_mode_t strip_color_mode;/*!< Intermediate strip pixel format. Determines the JPEG
                                               decode output and the PPA SRM input color mode.
                                               RGB565 halves ring memory vs RGB888; YUV420 needs a
                                               YUV420 source JPEG (no transcode in hardware). */
    jpeg_dec_rgb_element_order_t rgb_order;/*!< Element order of the decoded strips (RGB modes) */
    jpeg_yuv_rgb_conv_std_t conv_std;     /*!< BT601 or BT709 for the decode-side YUV->RGB CSC */
    bool yuv_full_range;                  /*!< Full-range (JFIF) decode matrix; see jpeg_enh_decode_cfg_t */
    uint32_t strip_alloc_caps;            /*!< heap_caps for the ring buffers; 0 = internal DMA RAM.
                                               MALLOC_CAP_SPIRAM trades speed for internal SRAM. */

    uint32_t worker_stack_size;           /*!< PPA worker task stack (0 = 4096) */
    uint32_t worker_priority;             /*!< PPA worker task priority (0 = 17) */
    int worker_core;                      /*!< PPA worker core affinity (0 / 1 / -1 = no affinity) */
} jpeg_ppa_pipeline_cfg_t;

typedef struct {
    uint32_t x, y, w, h;
} jpeg_ppa_rect_t;

/**
 * @brief Per-frame transform. All fields may change on every process() call.
 *        Zero-initialized = render the full image 1:1 at the output origin
 *        (scale 0 is treated as 1.0).
 */
typedef struct {
    ppa_srm_rotation_angle_t rotation;    /*!< PPA_SRM_ROTATION_ANGLE_{0,90,180,270} */
    float scale_x;                        /*!< Horizontal scale (input x axis); 0 = 1.0 */
    float scale_y;                        /*!< Vertical scale (input y axis); 0 = 1.0 */
    bool mirror_x;                        /*!< Mirror the rendered image horizontally (output space) */
    bool mirror_y;                        /*!< Mirror the rendered image vertically (output space) */
    bool rgb_swap;                        /*!< PPA input RGB<->BGR swap */
    bool byte_swap;                       /*!< PPA input byte swap (RGB565/ARGB8888 strips only) */
    jpeg_ppa_rect_t in_crop;              /*!< Input-space crop; w==0 or h==0 = full (valid) image.
                                               Pixels outside origin_w/origin_h are always cropped. */
    uint32_t out_offset_x;                /*!< Top-left of the rendered rect in the output picture */
    uint32_t out_offset_y;
} jpeg_ppa_transform_t;

/**
 * @brief Per-frame output description.
 */
typedef struct {
    void *buffer;                         /*!< Output frame buffer (PPA alignment rules apply) */
    size_t buffer_size;                   /*!< 0 = pic_w * pic_h * bytes_per_pixel */
    uint32_t pic_w;                       /*!< Output picture width (pixels) */
    uint32_t pic_h;                       /*!< Output picture height (pixels) */
    ppa_srm_color_mode_t color_mode;      /*!< Output pixel format */
    ppa_color_range_t yuv_range;          /*!< Output YUV range (YUV output modes only) */
    ppa_color_conv_std_rgb_yuv_t yuv_std; /*!< Output RGB->YUV standard (YUV output modes only) */
} jpeg_ppa_output_t;

/**
 * @brief Create the pipeline: strip decoder (Layer 1), strip ring, PPA SRM
 *        client and worker task.
 */
esp_err_t jpeg_ppa_pipeline_new(const jpeg_ppa_pipeline_cfg_t *cfg,
                                jpeg_ppa_pipeline_handle_t *out_handle);

/**
 * @brief Tear down the pipeline and release all resources.
 */
esp_err_t jpeg_ppa_pipeline_del(jpeg_ppa_pipeline_handle_t handle);

/**
 * @brief Decode one JPEG and render it into `out->buffer` with `transform`.
 *        Blocks until every strip has been pushed through PPA.
 *
 * Pixels of the output picture outside the rendered rect are left untouched.
 *
 * @param transform NULL = identity (full image, no rotation/scale, origin 0,0)
 * @param info      Optional; receives the frame geometry
 */
esp_err_t jpeg_ppa_pipeline_process(jpeg_ppa_pipeline_handle_t handle,
                                    const void *jpeg_data, size_t jpeg_size,
                                    const jpeg_ppa_output_t *out,
                                    const jpeg_ppa_transform_t *transform,
                                    jpeg_enh_frame_info_t *info);

#ifdef __cplusplus
}
#endif
