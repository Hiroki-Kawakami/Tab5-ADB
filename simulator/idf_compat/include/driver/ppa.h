// Host (simulator) shim for the ESP-IDF PPA (Pixel-Processing Accelerator) driver
// API (driver/ppa.h). The device build pulls the real header from esp_driver_ppa
// and the P4 runs the operations on the PPA HW; here the same API is backed by a
// software implementation (see src/ppa.c) so app code that offloads
// scale/rotate/mirror/blend/fill to the PPA can be developed on the desktop and
// the simulator previews the equivalent result.
//
// Per the idf_compat rule this is an Espressif-defined API, so the host
// implements the IDF API *itself* rather than re-abstracting it. The struct /
// enum surface is kept in sync with ESP-IDF v5.4.3; software limitations of the
// host implementation are documented at the top of src/ppa.c.
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "hal/ppa_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enumeration of all PPA available operations
 */
typedef enum {
    PPA_OPERATION_SRM,              /*!< Do scale-rotate-mirror operation */
    PPA_OPERATION_BLEND,            /*!< Do blend operation */
    PPA_OPERATION_FILL,             /*!< Do fill operation, use one constant pixel to fill a target window */
    PPA_OPERATION_INVALID,          /*!< Invalid PPA operations, indicates the quantity of available PPA operations */
} ppa_operation_t;

/**
 * @brief Type of PPA client handle
 */
typedef struct ppa_client_t *ppa_client_handle_t;

/**
 * @brief A collection of configuration items that used for registering a PPA client
 */
typedef struct {
    ppa_operation_t oper_type;                  /*!< The desired PPA operation for the client */
    uint32_t max_pending_trans_num;             /*!< The maximum number of pending transactions for the client. Defaults to 1. */
    ppa_data_burst_length_t data_burst_length;  /*!< The desired data burst length for all the transactions of the client. Defaults to PPA_DATA_BURST_LENGTH_128. */
} ppa_client_config_t;

/**
 * @brief Register a PPA client to do a specific PPA operation
 */
esp_err_t ppa_register_client(const ppa_client_config_t *config, ppa_client_handle_t *ret_client);

/**
 * @brief Unregister a PPA client
 */
esp_err_t ppa_unregister_client(ppa_client_handle_t ppa_client);

/**
 * @brief Type of PPA event data
 */
typedef struct {
} ppa_event_data_t;

/**
 * @brief Type of PPA event callback
 */
typedef bool (*ppa_event_callback_t)(ppa_client_handle_t ppa_client, ppa_event_data_t *event_data, void *user_data);

/**
 * @brief Group of supported PPA callbacks
 */
typedef struct {
    ppa_event_callback_t on_trans_done;     /*!< Invoked when a PPA transaction finishes */
} ppa_event_callbacks_t;

/**
 * @brief Register event callbacks for a PPA client
 */
esp_err_t ppa_client_register_event_callbacks(ppa_client_handle_t ppa_client, const ppa_event_callbacks_t *cbs);

/**
 * @brief A collection of configuration items for an input picture and the target block inside the picture
 */
typedef struct {
    const void *buffer;                     /*!< Pointer to the input picture buffer */
    uint32_t pic_w;                         /*!< Input picture width (unit: pixel) */
    uint32_t pic_h;                         /*!< Input picture height (unit: pixel) */
    uint32_t block_w;                       /*!< Target block width (unit: pixel) */
    uint32_t block_h;                       /*!< Target block height (unit: pixel) */
    uint32_t block_offset_x;                /*!< Target block offset in x direction in the picture (unit: pixel) */
    uint32_t block_offset_y;                /*!< Target block offset in y direction in the picture (unit: pixel) */
    union {
        ppa_srm_color_mode_t srm_cm;        /*!< Color mode of the picture in a PPA SRM operation */
        ppa_blend_color_mode_t blend_cm;    /*!< Color mode of the picture in a PPA blend operation */
        ppa_fill_color_mode_t fill_cm;      /*!< Color mode of the picture in a PPA fill operation */
    };
    ppa_color_range_t yuv_range;            /*!< When the color mode is any YUV color space, describes its color range */
    ppa_color_conv_std_rgb_yuv_t yuv_std;   /*!< When the color mode is any YUV color space, describes its YUV<->RGB conversion standard */
} ppa_in_pic_blk_config_t;

/**
 * @brief A collection of configuration items for an output picture and the target block inside the picture
 */
typedef struct {
    void *buffer;                           /*!< Pointer to the output picture buffer */
    uint32_t buffer_size;                   /*!< Size of the output picture buffer */
    uint32_t pic_w;                         /*!< Output picture width (unit: pixel) */
    uint32_t pic_h;                         /*!< Output picture height (unit: pixel) */
    uint32_t block_offset_x;                /*!< Target block offset in x direction in the picture (unit: pixel) */
    uint32_t block_offset_y;                /*!< Target block offset in y direction in the picture (unit: pixel) */
    union {
        ppa_srm_color_mode_t srm_cm;        /*!< Color mode of the picture in a PPA SRM operation */
        ppa_blend_color_mode_t blend_cm;    /*!< Color mode of the picture in a PPA blend operation */
        ppa_fill_color_mode_t fill_cm;      /*!< Color mode of the picture in a PPA fill operation */
    };
    ppa_color_range_t yuv_range;            /*!< When the color mode is any YUV color space, describes its color range */
    ppa_color_conv_std_rgb_yuv_t yuv_std;   /*!< When the color mode is any YUV color space, describes its YUV<->RGB conversion standard */
} ppa_out_pic_blk_config_t;

/**
 * @brief Modes to perform the PPA operations
 */
typedef enum {
    PPA_TRANS_MODE_BLOCKING,        /*!< `ppa_do_xxx` blocks until the PPA operation is finished */
    PPA_TRANS_MODE_NON_BLOCKING,    /*!< `ppa_do_xxx` returns immediately after the operation is queued */
} ppa_trans_mode_t;

/**
 * @brief A collection of configuration items to do a PPA SRM operation transaction
 */
typedef struct {
    ppa_in_pic_blk_config_t in;                 /*!< Information of the input picture and the target block */
    ppa_out_pic_blk_config_t out;               /*!< Information of the output picture and the target block */

    // scale-rotate-mirror manipulation
    ppa_srm_rotation_angle_t rotation_angle;    /*!< Rotation (counter-clockwise) to the target block */
    float scale_x;                              /*!< Scaling factor to the target block in the x direction */
    float scale_y;                              /*!< Scaling factor to the target block in the y direction */
    bool mirror_x;                              /*!< Whether to mirror the target block in the x direction */
    bool mirror_y;                              /*!< Whether to mirror the target block in the y direction */

    // input data manipulation
    bool rgb_swap;                              /*!< Whether to swap the input data in RGB (e.g. RGB becomes BGR) */
    bool byte_swap;                             /*!< Whether to swap the input data in byte. Only for ARGB8888 or RGB565 */
    ppa_alpha_update_mode_t alpha_update_mode;  /*!< Select whether the alpha channel of the input picture needs update */
    union {
        uint32_t alpha_fix_val;                 /*!< Range [0,255]. New alpha when PPA_ALPHA_FIX_VALUE. */
        float alpha_scale_ratio;                /*!< Range (0,1). Multiplier when PPA_ALPHA_SCALE. Resolution 1/256. */
    };

    ppa_trans_mode_t mode;                      /*!< Determines whether to block inside the operation function */
    void *user_data;                            /*!< User data passed into the on_trans_done callback */
} ppa_srm_oper_config_t;

/**
 * @brief Perform a scaling-rotating-mirroring (SRM) operation to a picture
 */
esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t ppa_client, const ppa_srm_oper_config_t *config);

/**
 * @brief A collection of configuration items to do a PPA blend operation transaction
 */
typedef struct {
    ppa_in_pic_blk_config_t in_bg;                 /*!< Information of the input background picture and the target block */
    ppa_in_pic_blk_config_t in_fg;                 /*!< Information of the input foreground picture and the target block */
    ppa_out_pic_blk_config_t out;                  /*!< Information of the output picture and the target block */

    // input data manipulation
    bool bg_rgb_swap;                              /*!< Whether to swap the background input data in RGB */
    bool bg_byte_swap;                             /*!< Whether to swap the background input data in byte (ARGB8888/RGB565 only) */
    ppa_alpha_update_mode_t bg_alpha_update_mode;  /*!< Select whether the alpha channel of the input background picture needs update */
    union {
        uint32_t bg_alpha_fix_val;                 /*!< Range [0,255]. */
        float bg_alpha_scale_ratio;                /*!< Range (0,1). */
    };
    bool fg_rgb_swap;                              /*!< Whether to swap the foreground input data in RGB */
    bool fg_byte_swap;                             /*!< Whether to swap the foreground input data in byte (ARGB8888/RGB565 only) */
    ppa_alpha_update_mode_t fg_alpha_update_mode;  /*!< Select whether the alpha channel of the input foreground picture needs update */
    union {
        uint32_t fg_alpha_fix_val;                 /*!< Range [0,255]. */
        float fg_alpha_scale_ratio;                /*!< Range (0,1). */
    };
    color_pixel_rgb888_data_t fg_fix_rgb_val;      /*!< When in_fg.blend_cm is A8/A4, the fixed RGB888 color for the foreground */

    // color-keying
    bool bg_ck_en;                                 /*!< Whether to enable color keying for background */
    color_pixel_rgb888_data_t bg_ck_rgb_low_thres; /*!< Lower threshold of the BG color-keying range (RGB888) */
    color_pixel_rgb888_data_t bg_ck_rgb_high_thres;/*!< Higher threshold of the BG color-keying range (RGB888) */
    bool fg_ck_en;                                 /*!< Whether to enable color keying for foreground */
    color_pixel_rgb888_data_t fg_ck_rgb_low_thres; /*!< Lower threshold of the FG color-keying range (RGB888) */
    color_pixel_rgb888_data_t fg_ck_rgb_high_thres;/*!< Higher threshold of the FG color-keying range (RGB888) */
    color_pixel_rgb888_data_t ck_rgb_default_val;  /*!< Color to overwrite when both BG and FG are within their color-keying ranges */
    bool ck_reverse_bg2fg;                         /*!< Output the FG element instead of the BG element for the keyed case */

    ppa_trans_mode_t mode;                         /*!< Determines whether to block inside the operation function */
    void *user_data;                               /*!< User data passed into the on_trans_done callback */
} ppa_blend_oper_config_t;

/**
 * @brief Perform a blending operation to a picture
 */
esp_err_t ppa_do_blend(ppa_client_handle_t ppa_client, const ppa_blend_oper_config_t *config);

/**
 * @brief A collection of configuration items to do a PPA fill operation transaction
 */
typedef struct {
    ppa_out_pic_blk_config_t out;                  /*!< Information of the output picture and the target block */

    uint32_t fill_block_w;                         /*!< The width of the block to be filled (unit: pixel) */
    uint32_t fill_block_h;                         /*!< The height of the block to be filled (unit: pixel) */
    color_pixel_argb8888_data_t fill_argb_color;   /*!< The color to be filled, in ARGB8888 format */

    ppa_trans_mode_t mode;                         /*!< Determines whether to block inside the operation function */
    void *user_data;                               /*!< User data passed into the on_trans_done callback */
} ppa_fill_oper_config_t;

/**
 * @brief Perform a filling operation to a picture
 */
esp_err_t ppa_do_fill(ppa_client_handle_t ppa_client, const ppa_fill_oper_config_t *config);

#ifdef __cplusplus
}
#endif
