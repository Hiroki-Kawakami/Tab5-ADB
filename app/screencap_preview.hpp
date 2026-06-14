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
#include "png_decode.hpp"  // PsramAllocator + decode_png_downscale_rgb565

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
    // `image` is an existing lv_image the preview renders into; max_w/max_h is the
    // bounding box the preview aspect-fits into. Each frame is decoded at the
    // fitted size derived from the source dimensions (so a rotated device shrinks
    // to a landscape frame) and the lv_image is resized to hug it — no letterbox.
    // All LVGL touches happen on the LVGL thread.
    static std::shared_ptr<ScreencapPreview> create(lv_obj_t* image, int max_w, int max_h);
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
    ScreencapPreview(lv_obj_t* image, int max_w, int max_h);

    void capture_once();              // LVGL thread: open the exec:screencap stream
    void schedule_next();             // LVGL thread: arm the interval timer
    void present(int idx, bool ok);   // LVGL thread: flip dsc_ to the freshly-decoded buffer

    static void decode_trampoline(void* arg);
    void decode_loop();               // decode task (Core 1, low prio): PNG/JPEG -> img_buf_[back]
    bool decode_jpeg(const uint8_t* data, size_t len, int idx);  // decode task

    lv_obj_t* image_;
    const int max_w_, max_h_;
    int interval_ms_ = 2000;

    lv_image_dsc_t dsc_{};            // points at the shown img_buf_
    // Double buffer (each max_w*max_h*2 RGB565, PSRAM — allocated once for the
    // bounding box; each frame uses a tightly-packed frame_w_*frame_h_ prefix):
    // the decode task writes the back buffer while LVGL shows the front, then
    // present() flips dsc_ to it — no per-frame copy. write_idx_ = the back
    // buffer (front = write_idx_ ^ 1).
    uint8_t* img_buf_[2] = {nullptr, nullptr};
    size_t buf_size_ = 0;             // bytes in each img_buf_
    std::atomic<int> write_idx_{1};   // back buffer the decode task targets next
    // Aspect-fitted size of the frame in each img_buf_ — written by the decode
    // task, read by present() on the LVGL thread. The pipeline is serial (the
    // next capture only arms after present), so no extra synchronization.
    int frame_w_[2] = {0, 0};
    int frame_h_[2] = {0, 0};

    // Compressed PNG accumulated on the reader thread (PSRAM — can be ~MBs).
    std::vector<uint8_t, PsramAllocator<uint8_t>> png_;
    std::shared_ptr<adb::Stream> stream_;
    lv_timer_t* timer_ = nullptr;     // one-shot, LVGL thread

    // Decode/downscale runs on a dedicated low-priority task pinned to Core 1, so
    // the heavy inflate never preempts LVGL or the high-priority adb reader. The
    // reader hands off the captured PNG via work_sem_ and returns immediately.
    void* decode_task_ = nullptr;     // TaskHandle_t
    void* work_sem_ = nullptr;        // binary: a captured image is ready to decode
    void* decode_done_ = nullptr;     // binary: the decode task has exited (join)
    std::atomic<bool> decode_stop_{false};

    // JPEG path: prefer `screencap -j` (UltraHDR base JPEG, ~5-8x smaller than PNG
    // on photo-heavy screens), falling back to `-p` (PNG) when the device lacks
    // `-j`. decode_loop auto-detects the format by magic bytes; use_jpeg_ flips off
    // permanently if a `-j` capture comes back non-JPEG.
    //
    // Disabled by default: `screencap -j` only exists on Android 16+, and no test
    // device on hand has it. The whole JPEG
    // path is built and validated, just gated off — flip this to true once an
    // Android 16 device is available to verify it for real.
    std::atomic<bool> use_jpeg_{false};
    void* jpeg_pipe_ = nullptr;       // jpeg_ppa_pipeline_handle_t (lazy, decode task)
    uint32_t jpeg_pipe_max_ = 0;      // max_pic dimension the pipeline was built for
    const char* last_fmt_ = "?";      // "jpeg"/"png" for the timing log

    std::atomic<bool> stopped_{false};
    bool capturing_ = false;          // LVGL thread: a capture is in flight

    // Profiling timestamps (µs) for the per-stage timing log — written as the frame
    // moves capture (LVGL) -> recv (reader) -> decode (task) -> present (LVGL).
    int64_t t_capture_us_ = 0;        // stream opened
    int64_t t_recv_us_ = 0;           // last PNG byte received (stream closed)
    int64_t t_dec_start_us_ = 0;      // decode task picked up the work
    int64_t t_dec_end_us_ = 0;        // decode+downscale finished
    int src_w_ = 0, src_h_ = 0;       // source PNG dimensions (for the log)
    size_t png_bytes_ = 0;            // last PNG size
};
