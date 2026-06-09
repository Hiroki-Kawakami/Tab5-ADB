#pragma once
#include <cstdint>
#include <memory>

#include "adb_app.hpp"     // PANEL_W, PANEL_H
#include "agent_link.hpp"  // agent_link::Link, agent_link::LinkListener
#include "driver/jpeg_decode.h"  // jpeg_decoder_handle_t (IDF on device, shim on host)
#include "screen.hpp"

// Screen-mirror viewer. Launches tab5adb-agent on the connected device (pushes
// the embedded jar, app_process), opens an agent_link::Link, sends MIRROR_START,
// and renders the incoming JPEG strip stream onto the panel.
//
// Rendering goes STRAIGHT to the panel, bypassing LVGL: the agent always sends
// full-panel-width frames (scale-fit + letterbox are done agent-side), so each
// JPEG strip is the full panel width and HW-JPEG-decodes tightly-packed straight
// into its framebuffer row band — its width equals the framebuffer pitch, so
// "packed" is already "in place" with no scratch buffer, blit, or stride (the P4
// 2D-DMA can't place a narrower picture into a wider buffer). The finished frame
// is presented with bsp_display_flush(). The three bsp framebuffers are used as a
// triple buffer (decode into the back one, flush it, advance to the next), so the
// displayed buffer is never being written and the decode task never waits on the
// panel scan-out/vsync sync before reusing a buffer — tear-free with no per-frame
// LVGL compositing at all. While the mirror screen is active its LVGL root is static (a black,
// clickable surface) so lv_timer_handler does not touch the framebuffers; on pop,
// the previous screen's full re-render takes them back.
//
// The screen IS the agent_link::LinkListener; the multi-step launch + the retry
// until the agent is listening run on a private MirrorLauncher worker task.
//
// Receive and decode run on SEPARATE threads so a strip's HW-JPEG decode never
// stalls the adb reader thread (and thus the per-A_WRTE/A_OKAY flow control that
// gates the next USB IN transfer): on the reader thread on_video_strip only
// copies each strip's JPEG bytes into a frame slot and, on frame_end, hands the
// finished frame to a private decode task; the reader then immediately acks and
// keeps the USB stream flowing while the decode task HW-decodes into the
// framebuffer in parallel. Slots are passed by ownership through two queues
// (free_q_ -> producer fills -> ready_q_ -> consumer decodes -> free_q_), so a
// slot is touched by exactly one thread at a time and the producer drops whole
// frames when the consumer falls behind (latest-frame-wins). NO LVGL widgets are
// composited over the stream (a back button / status label would force LVGL to
// re-blend every frame — the draw cost we avoid): navigation back is a tap on the
// root, launch progress goes to the log.
class MirrorLauncher;

class ADBMirroringScreen : public Screen, public agent_link::LinkListener {
public:
    ADBMirroringScreen();
    ~ADBMirroringScreen() override;

    void build() override;
    void onEnter() override;  // enter DM overlay mode + build the control bar
    void onExit() override;   // stop the launcher (closes the link) before destroy

    // agent_link::LinkListener — on_video_strip fires on the adb reader thread.
    void on_link_hello(agent_link::Link* link, const agent_link::AgentInfo& info) override;
    void on_mirror_started(agent_link::Link* link, const agent_link::MirrorInfo& info) override;
    void on_video_strip(agent_link::Link* link, const agent_link::VideoStrip& strip) override;
    void on_link_close(agent_link::Link* link, adb::Error err) override;

private:
    // The smallest strip is 16px tall, so a panel holds at most this many strips.
    static constexpr int kMaxStrips = PANEL_H / 16;
    // Frame slots in flight: the consumer holds the decoding slot AND retains the
    // last decoded frame (to re-decode and erase the bar when the overlay is
    // hidden over static video), the producer holds the filling one, and ready_q_
    // holds at most one — plus headroom so a steady stream never drops spuriously.
    static constexpr int kSlots = 5;
    // Opaque control bar: a full-width strip across the bottom of the panel.
    static constexpr int kBarH = 96;

    // One received frame: its strips' JPEG bytes concatenated in `buf` (PSRAM,
    // grown lazily) plus a descriptor per strip. The reader thread fills it; the
    // decode task reads it. Ownership moves between them via free_q_/ready_q_, so
    // only one thread touches a given slot at a time (no lock).
    struct StripDesc { uint16_t y, h; uint32_t off, len; };
    struct FrameSlot {
        uint8_t* buf = nullptr;   // PSRAM; decoded straight from (no DMA-copy needed)
        uint32_t cap = 0;
        uint32_t write_off = 0;
        StripDesc strips[kMaxStrips];
        int strip_count = 0;
        uint64_t bytes = 0;       // total JPEG bytes (stats)
    };

    // Decode task (consumer): drains ready_q_, HW-decodes each frame's strips into
    // the back framebuffer, presents it, recycles the slot to free_q_.
    static void decode_trampoline(void* arg);
    void decode_loop();
    // Decode one full-width strip straight into framebuffer `dst`, packed at row
    // `y` (decode task). Lazily creates the JPEG engine. Returns false on a decode
    // error or a non-full-width strip.
    bool decode_one(uint8_t* jpeg, uint32_t len, uint16_t y, uint16_t h,
                    uint16_t* dst);
    void free_decoder();
    // LVGL-thread lv_timer callback: poll the raw touch and toggle the overlay
    // bar when the video area (outside the bar) is tapped.
    void poll_touch();

    // The bsp framebuffers (not owned), used as a TRIPLE buffer: the decode task
    // draws into fb_[back_], flushes it (it becomes the front/displayed buffer),
    // then advances back_ to the next buffer in the rotation. With three buffers
    // the buffer drawn into was last displayed two frames ago, so the decode task
    // never waits on the panel's scan-out/vsync sync before reusing one.
    static constexpr int kFbCount = 3;
    uint16_t* fb_[kFbCount] = {nullptr, nullptr, nullptr};
    int back_ = 0;     // index the decode task draws into next
    int front_ = -1;   // index currently displayed (last flushed; -1 = none yet)

    void* poll_timer_ = nullptr;  // lv_timer_t* (LVGL thread): bar show/hide toggle
    bool  touch_prev_ = false;    // previous press state, for tap-edge detection

    FrameSlot slots_[kSlots];
    void* free_q_ = nullptr;   // QueueHandle_t<int>: slot indices the producer may fill
    void* ready_q_ = nullptr;  // QueueHandle_t<int>, cap 1: the latest finished frame
    int fill_slot_ = -1;       // slot the reader thread is currently filling (-1 = none)
    bool fill_bad_ = false;    // current frame overflowed / had a bad strip → drop it

    void* decode_task_ = nullptr;
    void* decode_done_ = nullptr;     // binary sem given when the decode task exits
    volatile bool decode_stop_ = false;

    // FPS instrumentation (decode task only): presented frames / strips / JPEG
    // bytes, logged roughly once per second.
    int64_t stats_start_us_ = 0;  // start of the current ~1s window (0 = not started)
    uint32_t stats_frames_ = 0;
    uint32_t stats_strips_ = 0;
    uint64_t stats_bytes_ = 0;

    // JPEG decode (decode task only) — the device/host-shared seam
    // (jpeg_fullrange_decode); strips land straight in the framebuffer, so there
    // is no decoded-pixel scratch and the slot buffer is the compressed input.
    jpeg_decoder_handle_t jpeg_ = nullptr;

    std::shared_ptr<MirrorLauncher> launcher_;
};
