#pragma once
#include "esp_err.h"
#include "driver/jpeg_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Drop-in replacement for the IDF jpeg_decoder_process() that performs
 * *full-range* (JFIF / "PC range", Y/Cb/Cr all in [0,255]) BT.601 YCbCr->RGB
 * color conversion instead of the IDF default *limited-range* (Y in [16,235])
 * BT.601 form.
 *
 * MJPEG / JFIF content is encoded full-range, so the limited-range matrix the
 * IDF driver bakes into the 2D-DMA CSC unit under-saturates the decoded image
 * (washed-out blacks, compressed highlights). This re-runs the decode flow but
 * overwrites the CSC matrix registers with the full-range coefficients right
 * before the transfer starts.
 *
 * The engine is still created/destroyed with the standard IDF
 * jpeg_new_decoder_engine() / jpeg_del_decoder_engine(); only the per-frame
 * process step is reimplemented here. The signature mirrors
 * jpeg_decoder_process() exactly so it can be swapped in directly.
 *
 * Only BT.601 RGB565/RGB888 output is range-adjusted; BT.709 and YUV/GRAY
 * outputs behave identically to the IDF driver.
 */
esp_err_t jpeg_decoder_process_full_range(
    jpeg_decoder_handle_t decoder_engine,
    const jpeg_decode_cfg_t *decode_cfg,
    const uint8_t *bit_stream, uint32_t stream_size,
    uint8_t *decode_outbuf, uint32_t outbuf_size,
    uint32_t *out_size);

#ifdef __cplusplus
}
#endif
