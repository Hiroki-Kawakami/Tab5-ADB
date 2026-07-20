#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "adb.hpp"  // adb::Sync, adb::SyncListener, adb::Error
#include "file_preview.hpp"   // app::FileRef + the preview building blocks
#include "file_transfer.hpp"  // app::TransferJob
#include "psram_allocator.hpp"
#include "screen.hpp"

// Image preview — the ".jpg"/".jpeg"/".png" entries in the file-preview registry
// (both SD card and Android device). Decodes the picture and shows it aspect-fit
// in the stage, then offers the same copy actions as the generic preview.
//
// A low-priority Core-1 task streams image_framework rows through its resizer
// into a small RGB565 frame. The full image is never materialized at native
// resolution.
class ImagePreviewScreen : public Screen, public adb::SyncListener {
public:
    explicit ImagePreviewScreen(app::FileRef ref) : ref_(std::move(ref)) {}
    ~ImagePreviewScreen() override;

    void build() override;
    void onExit() override;  // close the sync session + join the decode task, abort copy

    // adb::SyncListener — fires on the Sync worker thread.
    void on_sync_close(adb::Sync* s, adb::Error err) override {}

private:
    void start_load();         // LVGL thread: spinner + kick the load/decode
    void present(bool ok);     // LVGL thread: show the decoded frame (or an error)
    void stop_engine();        // close the sync session + join the decode task

    void rebuild_actions(lv_obj_t* box);  // LVGL thread: copy-to-* rows
    void copy_to_sd();
    void copy_to_android();

    static void decode_trampoline(void* arg);
    void decode_loop();        // decode task (Core 1, low prio): bytes -> img_buf_
    bool load_sd_file();       // decode task: read the SD file into data_
    bool decode_image(const uint8_t* data, size_t len);  // decode task

    // Bounding box the preview aspect-fits into (the content column minus padding;
    // the decoded RGB565 frame stays well under a native-resolution photo).
    static constexpr int kMaxW = 660;
    static constexpr int kMaxH = 980;

    app::FileRef ref_;

    // ---- UI (LVGL thread) ----
    lv_obj_t* stage_{nullptr};   // centers the spinner / image / error label
    lv_obj_t* image_{nullptr};   // the decoded preview

    // ---- preview buffer ----
    lv_image_dsc_t dsc_{};
    uint8_t* img_buf_{nullptr};  // RGB565, kMaxW*kMaxH*2, PSRAM
    size_t buf_size_{0};
    int frame_w_{0}, frame_h_{0};
    int src_w_{0}, src_h_{0};
    bool oversize_{false};  // file won't fit in PSRAM -> "too large" error, not a crash

    // ---- load / decode ----
    std::shared_ptr<adb::Sync> sync_;  // Android source (sync: RECV)
    // Raw compressed image bytes (PSRAM — a photo can be several MB). The SD path
    // and the Sync RECV sink fill it on the worker thread; the decode task reads it
    // after work_sem_ (a happens-before, so no lock between producer and consumer).
    std::vector<uint8_t, PsramAllocator<uint8_t>> data_;

    void* decode_task_{nullptr};  // TaskHandle_t
    void* work_sem_{nullptr};     // bytes are ready to decode
    void* decode_done_{nullptr};  // decode task exited (join)
    std::atomic<bool> decode_stop_{false};

    // ---- copy actions ----
    std::shared_ptr<app::TransferJob> job_;
};
