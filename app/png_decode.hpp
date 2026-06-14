#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "esp_heap_caps.h"

// std::vector allocator that places storage in PSRAM (MALLOC_CAP_SPIRAM) — a
// screen-capture PNG can be a couple of MB, which won't fit the P4's internal
// SRAM. On the host simulator heap_caps_malloc ignores the caps (it's plain
// malloc), so this is portable across both targets.
template <class T>
struct PsramAllocator {
    using value_type = T;
    PsramAllocator() = default;
    template <class U>
    PsramAllocator(const PsramAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
        if (!p) abort();  // OOM; device builds with -fno-exceptions, so can't throw
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept { heap_caps_free(p); }
};
template <class A, class B>
bool operator==(const PsramAllocator<A>&, const PsramAllocator<B>&) noexcept { return true; }
template <class A, class B>
bool operator!=(const PsramAllocator<A>&, const PsramAllocator<B>&) noexcept { return false; }

namespace app {

// Aspect-fit src_w x src_h into the max_w x max_h bounding box.
void aspect_fit(int src_w, int src_h, int max_w, int max_h, int* fit_w, int* fit_h);

// Decode a PNG and downscale it on the fly, aspect-fitted into a max_w*max_h
// bounding box, as a tightly-packed fit_w*fit_h RGB565 frame at the start of
// `out` (which must hold max_w*max_h uint16). The full-resolution image is
// never materialized: PNG row filters only reference the *previous*
// reconstructed row, so the image is inflated row by row keeping just two
// scanlines and the kept (nearest-neighbour) rows are emitted straight into
// `out`. This is what lets a large device screenshot be previewed without ever
// handing LVGL a native-resolution image. Constraints (all met by Android
// `screencap -p`): 8-bit, colour type RGB(2)/RGBA(6), non-interlaced. Returns
// false otherwise. `src_w`/`src_h` (optional) receive the source dimensions.
bool decode_png_downscale_rgb565(const uint8_t* png, size_t len,
                                 uint16_t* out, int max_w, int max_h,
                                 int* fit_w, int* fit_h,
                                 int* src_w = nullptr, int* src_h = nullptr);

}  // namespace app
