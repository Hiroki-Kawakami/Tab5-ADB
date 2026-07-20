#include "image_decode.hpp"

#include <memory>

#include "esp_heap_caps.h"
#include "imgf_alloc.h"
#include "imgf_decoder.h"
#include "imgf_resize.h"
#include "imgf_stream.h"

namespace app {

imgf_err_t decode_image_rgb565(const uint8_t* data, size_t len,
                               uint8_t* output, size_t output_size,
                               int max_w, int max_h,
                               ImageDecodeResult* result) {
    if (!data || !output || !result || max_w <= 0 || max_h <= 0 ||
        max_w > 0xffff || max_h > 0xffff) {
        return IMGF_ERR_INVALID_ARG;
    }

    imgf_format_t format = imgf_sniff(data, len < 8 ? len : 8);
    using Decoder = std::unique_ptr<imgf_decoder_t, decltype(&imgf_decoder_destroy)>;
    Decoder decoder(nullptr, imgf_decoder_destroy);
    imgf_buffer_source_t source;
    auto open_decoder = [&](bool downscale) {
        decoder.reset(imgf_make_decoder(format));
        if (!decoder) return imgf_err_t{IMGF_ERR_UNSUPPORTED};
        imgf_stream_t stream = imgf_stream_from_buffer(&source, data, len);
        imgf_decode_opts_t opts = {};
        if (downscale) {
            opts.target_w = max_w;
            opts.target_h = max_h;
        }
        opts.alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        return imgf_decoder_open(decoder.get(), stream, &opts);
    };

    imgf_err_t err = open_decoder(false);
    if (err != IMGF_OK) return err;

    int src_w = imgf_decoder_width(decoder.get());
    int src_h = imgf_decoder_height(decoder.get());
    if (format == IMGF_FMT_JPEG && src_w / 2 >= max_w && src_h / 2 >= max_h) {
        err = open_decoder(true);
        if (err != IMGF_OK) return err;
    }

    imgf_resize_opts_t opts = {};
    opts.target_w = max_w;
    opts.target_h = max_h;
    opts.fit = IMGF_FIT_CONTAIN;
    opts.dst_pixfmt = IMGF_PIX_RGB565;
    opts.alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    using Resizer = std::unique_ptr<imgf_resizer_t, decltype(&imgf_resizer_destroy)>;
    Resizer resizer(imgf_resizer_create(imgf_decoder_width(decoder.get()),
                                        imgf_decoder_height(decoder.get()),
                                        imgf_decoder_pixfmt(decoder.get()), &opts, &err),
                    imgf_resizer_destroy);
    if (!resizer) return err;

    int frame_w = imgf_resizer_dst_width(resizer.get());
    int frame_h = imgf_resizer_dst_height(resizer.get());
    if ((size_t)frame_w * frame_h * 2 > output_size) return IMGF_ERR_INVALID_ARG;

    int src_bpp = imgf_pixfmt_bpp(imgf_decoder_pixfmt(decoder.get()));
    size_t row_size = (size_t)imgf_decoder_width(decoder.get()) * src_bpp;
    using Buffer = std::unique_ptr<uint8_t, decltype(&imgf_free)>;
    Buffer row(static_cast<uint8_t*>(imgf_alloc(
                   row_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
               imgf_free);
    if (!row) return IMGF_ERR_OOM;

    size_t stride = (size_t)frame_w * 2;
    int out_row = 0;
    for (int y = 0; y < imgf_decoder_height(decoder.get()); y++) {
        if (!imgf_decoder_next_row(decoder.get(), row.get())) {
            err = imgf_decoder_last_error(decoder.get());
            return err != IMGF_OK ? err : IMGF_ERR_TRUNCATED;
        }
        int ready = imgf_resizer_push_row(resizer.get(), row.get());
        if (ready < 0) return imgf_resizer_last_error(resizer.get());
        for (int i = 0; i < ready && out_row < frame_h; i++) {
            if (!imgf_resizer_pop_row(resizer.get(), output + (size_t)out_row * stride))
                return IMGF_ERR_DECODE;
            out_row++;
        }
    }

    if (imgf_resizer_finish(resizer.get()) > 0 && out_row < frame_h) {
        if (!imgf_resizer_pop_row(resizer.get(), output + (size_t)out_row * stride))
            return IMGF_ERR_DECODE;
        out_row++;
    }
    if (out_row != frame_h) return IMGF_ERR_DECODE;

    result->format = format;
    result->src_w = src_w;
    result->src_h = src_h;
    result->frame_w = frame_w;
    result->frame_h = frame_h;
    return IMGF_OK;
}

}  // namespace app
