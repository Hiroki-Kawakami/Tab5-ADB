#pragma once
#include <tuple>
#include <optional>
#include <cstdint>
#include <cstddef>

namespace pf_port {

enum class Error {
    Ok,
    Fail,
    InvalidArgument,
};

enum class PixelFormat {
    RGB565,
    RGB888,
};

void init(int fb_num, PixelFormat pixel_format);
PixelFormat display_pixel_format();
inline size_t bytes_per_pixel(PixelFormat pf) {
    switch (pf) {
        case PixelFormat::RGB565: return 2;
        case PixelFormat::RGB888: return 3;
    }
    return 0;
}
void display_set_brightness(int value);
void *display_get_frame_buffer(int fb_index);
void display_flush(int fb_index);
std::optional<std::tuple<int, int>> touch_get_point();

void *psram_malloc(size_t size);
void *psram_malloc_dma(size_t size);

}
