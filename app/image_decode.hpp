#pragma once

#include <cstddef>
#include <cstdint>

#include "imgf_sniff.h"
#include "imgf_types.h"

namespace app {

struct ImageDecodeResult {
    imgf_format_t format{IMGF_FMT_UNKNOWN};
    int src_w{0};
    int src_h{0};
    int frame_w{0};
    int frame_h{0};
};

imgf_err_t decode_image_rgb565(const uint8_t* data, size_t len,
                               uint8_t* output, size_t output_size,
                               int max_w, int max_h,
                               ImageDecodeResult* result);

}  // namespace app
