#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "agent_link.hpp"          // agent_link::Link, VideoListener
#include "jpeg_decode_enhanced.h"  // jpeg_enh_strip_decoder_handle_t
#include "lvgl.hpp"

// The Normal-mode device-screen preview: a small live mirror of the phone over
// the tab5adb-agent JPEG stream, rendered into an lv_image. The agent-free
// `exec:screencap` fallback for Limited mode is ScreencapPreview; this one
// rides the established agent link instead, so it is much lighter (no per-frame
// PNG round trip) and runs at a real frame rate.
//
// Wire setup: start_mirror with scale_mode = aspect (§5.3) — the AGENT sizes the
// output to the source's natural aspect within the box_w×box_h request, so the
// frame fills the lv_image edge to edge with the ScreencapPreview behavior
// preserved: fixed 360 width, height following the phone's aspect (the chosen
// size arrives in the MIRROR_START response / the frame rect). split_count=1
// (each frame is ONE whole JPEG — no strip banding for a small frame, §5.3),
// jpeg_quality=60 and max_fps≈10 keep the stream light so it never crowds the
// link. With a single strip nothing needs 16px alignment, which is what allows
// the exact 360 width.
//
// Rendering is the mirror screen's receive/decode split scaled down to LVGL: the
// adb reader thread only copies each frame's JPEG into a slot and publishes it
// (latest-wins, never blocking the reader); a low-priority decode task drains
// frames through the jpeg_decode_enhanced whole-frame seam into a
// double-buffered RGB565 image, then one lv_async_call flips the lv_image to the
// finished buffer (the ScreencapPreview present pattern). The decoded raster is
// MCU-padded (rows stored at ceil16(w), jpeg_enh_frame_info_t.pic_w), so the
// buffers are allocated at the padded size, 64-byte aligned (the HW decoder's
// cache-line rule), and the lv_image shows the real w×h through the dsc stride.
// Since the video shows the natural-orientation framebuffer (§5.1), a rotated
// phone keeps its portrait frame here (the screencap preview instead followed
// the logical rotation).
//
// Threading: create/start/stop on the LVGL thread. stop() sends MIRROR_STOP but
// KEEPS the agent link (the AgentClient contract) and joins the decode task, so
// after it returns nothing touches the lv_image or the buffers.
class AgentPreview : public agent_link::VideoListener,
                     public std::enable_shared_from_this<AgentPreview> {
public:
    // `image` is an existing lv_image the preview renders into; box_w/box_h is
    // the (even) bounding box the agent sizes the stream into. The lv_image is
    // resized to hug each stream's frame (no letterbox), top-anchored by the
    // caller.
    static std::shared_ptr<AgentPreview> create(lv_obj_t* image, int box_w, int box_h);
    ~AgentPreview() override;

    // Register on the agent link and MIRROR_START. The agent must be Ready
    // (app::agent_client().ready()); call again after an ensure_connected if not.
    void start();
    // MIRROR_STOP (link kept) + detach + join the decode task. Idempotent.
    void stop();

    // agent_link::VideoListener — both fire on the adb reader thread.
    void on_mirror_started(agent_link::Link* link,
                           const agent_link::MirrorInfo& info) override;
    void on_video_strip(agent_link::Link* link,
                        const agent_link::VideoStrip& strip) override;

private:
    AgentPreview(lv_obj_t* image, int box_w, int box_h);

    static void decode_trampoline(void* arg);
    void decode_loop();     // decode task: slots -> img_buf_[back] -> present
    void present(int idx);  // LVGL thread: flip the lv_image to img_buf_[idx]

    lv_obj_t* image_;
    const int box_w_, box_h_;
    const int pad_w_, pad_h_;  // MCU-padded (ceil16) box — the buffer geometry

    // Double-buffered RGB565 output, sized once for the MCU-padded bounding box
    // and 64-byte aligned (the whole-frame decoder's cache rules); each frame is
    // a fw×fh image stored at the decoder's padded stride. The decode task
    // writes the back buffer while LVGL shows the front; present() flips.
    // present_pending_ keeps the decode task off a buffer whose flip is still
    // queued on the LVGL thread.
    lv_image_dsc_t dsc_{};
    uint8_t* img_buf_[2] = {nullptr, nullptr};
    size_t buf_size_ = 0;
    int back_ = 0;              // decode task's target buffer
    int frame_w_[2] = {0, 0};   // dims of the frame in each buffer
    int frame_h_[2] = {0, 0};
    int stride_[2] = {0, 0};    // bytes per row (MCU-padded pic_w * 2)
    std::atomic<bool> present_pending_{false};

    // The stream's output frame size (reader thread only): from the MIRROR_START
    // response; copied into each published slot.
    int out_w_ = 0, out_h_ = 0;

    // One received frame = one whole-frame JPEG (split_count=1), filled by the
    // reader thread, decoded by the decode task; ownership moves through
    // free_q_/ready_q_ (the mirror screen's slot scheme, latest-frame-wins).
    static constexpr int kSlots = 3;
    struct FrameSlot {
        uint8_t* buf = nullptr;  // PSRAM, grown lazily
        uint32_t cap = 0;
        uint32_t len = 0;
        uint16_t fw = 0, fh = 0;  // frame dims the JPEG decodes to
    };
    FrameSlot slots_[kSlots];
    void* free_q_ = nullptr;   // QueueHandle_t<int>
    void* ready_q_ = nullptr;  // QueueHandle_t<int>, cap 1 (latest wins)

    void* decode_task_ = nullptr;  // TaskHandle_t
    void* decode_done_ = nullptr;  // binary sem: decode task exited
    std::atomic<bool> decode_stop_{false};
    std::atomic<bool> stopped_{false};

    jpeg_enh_strip_decoder_handle_t jpeg_ = nullptr;  // decode task only
};
