#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "adb.hpp"  // adb::Stream, adb::StreamListener, adb::Error
#include "png_decode.hpp"  // PsramAllocator
#include "screen.hpp"

// One-shot device screenshot, reached from ADBDeviceScreen's "Screenshot" tool
// button. Opening the screen fires `exec:screencap -p` straight away and shows a
// centered spinner; when the PNG lands it is downscaled (on a worker task, never
// handed to LVGL at native resolution — a full screenshot bitmap is far too
// large for the P4 to render) into a small RGB565 preview, then a Save-to-SD
// (the *full-resolution* PNG) and a Re-capture button are shown.
//
// The capture/decode split mirrors ScreencapPreview (the agent-free preview):
// PNG bytes accumulate on the adb reader thread, a low-priority decode task
// inflates+downscales them, and present() flips the result onto an lv_image on
// the LVGL thread. Unlike the preview this is a single shot (no interval timer)
// and the full PNG is retained so Save writes the device-resolution image.
class ADBScreenshotScreen : public Screen, public adb::StreamListener {
public:
    ADBScreenshotScreen();
    ~ADBScreenshotScreen() override;

    void build() override;
    void onExit() override;  // close the stream + join the decode task

    // adb::StreamListener — both fire on the adb reader thread.
    void on_stream_data(adb::Stream* st, const uint8_t* data, size_t len) override;
    void on_stream_close(adb::Stream* st, adb::Error err) override;

private:
    void start_capture();      // LVGL thread: open the stream, show the spinner
    void show_capturing();     // LVGL thread: spinner in the stage, hide buttons
    void present(bool ok);     // LVGL thread: show the decoded frame (or an error)
    void save_to_sd();         // LVGL thread: write the full PNG to /sd
    void stop_engine();        // close the stream + join the decode task

    static void decode_trampoline(void* arg);
    void decode_loop();        // decode task (Core 1, low prio): PNG -> img_buf_

    // Bounding box the preview aspect-fits into (well under a full screenshot,
    // so the decoded RGB565 frame stays small).
    static constexpr int kMaxW = 660;
    static constexpr int kMaxH = 1000;

    // ---- UI (LVGL thread) ----
    lv_obj_t* stage_{nullptr};     // centers the spinner / image / error label
    lv_obj_t* image_{nullptr};     // the decoded preview
    lv_obj_t* buttons_{nullptr};   // Save / Re-capture row (hidden while capturing)
    lv_obj_t* save_btn_{nullptr};  // greyed until a shot is available
    lv_obj_t* save_icon_{nullptr};
    lv_obj_t* save_label_{nullptr};
    lv_obj_t* save_card_{nullptr}; // save-in-flight dialog (also the in-flight guard)

    // ---- preview buffer ----
    lv_image_dsc_t dsc_{};
    uint8_t* img_buf_{nullptr};    // RGB565, kMaxW*kMaxH*2, PSRAM
    size_t buf_size_{0};
    int frame_w_{0}, frame_h_{0};
    int src_w_{0}, src_h_{0};

    // ---- capture / decode ----
    std::shared_ptr<adb::Stream> stream_;
    // Full captured PNG (reader thread appends; decode task + Save read it). The
    // capture is serial — the next Re-capture only clears it after a present —
    // and Save snapshots it into its own buffer, so no lock is needed.
    std::vector<uint8_t, PsramAllocator<uint8_t>> png_;
    bool capturing_{false};        // LVGL thread: a capture is in flight
    bool have_shot_{false};        // a decoded screenshot is currently shown

    void* decode_task_{nullptr};   // TaskHandle_t
    void* work_sem_{nullptr};      // a captured PNG awaits decode
    void* decode_done_{nullptr};   // decode task exited (join)
    std::atomic<bool> decode_stop_{false};
};
