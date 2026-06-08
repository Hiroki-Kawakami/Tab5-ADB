// Host (simulator) implementation of jpeg_decoder_process_full_range().
//
// The device variant overrides the 2D-DMA CSC matrix registers to get full-range
// (JFIF) BT.601 YCbCr->RGB instead of the IDF default limited-range form. On the
// host there is no CSC unit: the idf_compat libjpeg shim (jpeg_decoder_process)
// already decodes JFIF full-range, so this wrapper is a plain passthrough and the
// simulator reproduces the same colors the device produces with the fix applied.

#include "jpeg_fullrange_decode.h"

esp_err_t jpeg_decoder_process_full_range(
    jpeg_decoder_handle_t decoder_engine,
    const jpeg_decode_cfg_t *decode_cfg,
    const uint8_t *bit_stream, uint32_t stream_size,
    uint8_t *decode_outbuf, uint32_t outbuf_size,
    uint32_t *out_size)
{
    return jpeg_decoder_process(decoder_engine, decode_cfg, bit_stream,
                                stream_size, decode_outbuf, outbuf_size, out_size);
}
