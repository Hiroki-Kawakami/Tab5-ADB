#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

#include "adb_raw_stream.hpp"
#include "esp_heap_caps.h"
#include "lvgl.hpp"

// std::vector allocator that places storage in PSRAM (MALLOC_CAP_SPIRAM) — the
// preview's compressed PNG can be a couple of MB, which won't fit the P4's
// internal SRAM. On the host simulator heap_caps_malloc ignores the caps (it's
// plain malloc), so this is portable across both targets.
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

// A low-rate, agent-free preview of the device screen for ADBDeviceScreen.
//
// Unlike the mirror (agent_link, high-fps, custom JPEG strip stream), this is a
// "rough glance" — one capture every ~2 s — and deliberately reuses plain ADB:
// it runs `exec:screencap -p` (PNG, binary-safe over exec:, not the CR/LF-
// mangling shell: PTY) and streams the bytes back over a generic adb::Stream.
//
// The win over raw RGBA (screencap with no -p) is bandwidth: a UI screenshot
// PNG is ~0.2-2 MB vs ~10 MB raw, so the preview doesn't eat the link the mirror
// needs. PNG's row filters only reference the *previous* reconstructed row, so
// decoding never materializes the full image: we hold the compressed PNG, then
// inflate row-by-row keeping just two scanlines and downscale on the fly into a
// small RGB565 buffer (see screencap_preview.cpp). Interlaced PNG would break the
// row-streaming, but screencap never emits it (we verify IHDR.interlace == 0).
//
// App-specific (depends on adb::Client::open_stream), so it lives in app/.
class ScreencapPreview : public adb::StreamListener,
                         public std::enable_shared_from_this<ScreencapPreview> {
public:
    // `image` is an existing lv_image the preview renders into; dst_w/dst_h is the
    // downscale target (the preview's pixel size). All LVGL touches happen on the
    // LVGL thread.
    static std::shared_ptr<ScreencapPreview> create(lv_obj_t* image, int dst_w, int dst_h);
    ~ScreencapPreview() override;

    // Begin the capture loop (chains a fresh capture every interval_ms_). Call on
    // the LVGL thread once the adb client is Online.
    void start();
    // Stop the loop and detach. Call on the LVGL thread (e.g. onExit). After this,
    // no further LVGL writes happen. Idempotent.
    void stop();

    // adb::StreamListener — both fire on the adb reader thread.
    void on_stream_data(adb::Stream* st, const uint8_t* data, size_t len) override;
    void on_stream_close(adb::Stream* st, adb::Error err) override;

private:
    ScreencapPreview(lv_obj_t* image, int dst_w, int dst_h);

    void capture_once();              // LVGL thread: open the exec:screencap stream
    void schedule_next();             // LVGL thread: arm the interval timer
    void present();                   // LVGL thread: push decode_buf_ into the image

    lv_obj_t* image_;
    const int dst_w_, dst_h_;
    int interval_ms_ = 2000;

    lv_image_dsc_t dsc_{};            // points at img_buf_ (shown)
    uint8_t* img_buf_ = nullptr;      // dst_w*dst_h*2 RGB565, PSRAM — read by LVGL
    uint8_t* decode_buf_ = nullptr;   // dst_w*dst_h*2 RGB565, PSRAM — written by reader thread
    size_t buf_size_ = 0;             // bytes in each of img_buf_/decode_buf_
    bool have_frame_ = false;         // decode_buf_ holds a fresh frame to present

    // Compressed PNG accumulated on the reader thread (PSRAM — can be ~MBs).
    std::vector<uint8_t, PsramAllocator<uint8_t>> png_;
    std::shared_ptr<adb::Stream> stream_;
    lv_timer_t* timer_ = nullptr;     // one-shot, LVGL thread

    std::atomic<bool> stopped_{false};
    bool capturing_ = false;          // LVGL thread: a capture is in flight
};
