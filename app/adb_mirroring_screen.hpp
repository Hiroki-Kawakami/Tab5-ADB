#pragma once
#include <memory>

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
// is presented with bsp_display_flush(). The two bsp framebuffers are used as a
// double buffer (decode into the back one, flush it, swap), so the displayed
// buffer is never being written — tear-free with no per-frame LVGL compositing at
// all. While the mirror screen is active its LVGL root is static (a black,
// clickable surface) so lv_timer_handler does not touch the framebuffers; on pop,
// the previous screen's full re-render takes them back.
//
// The screen IS the agent_link::LinkListener; the multi-step launch + the retry
// until the agent is listening run on a private MirrorLauncher worker task. Strip
// decode + flush happen on the adb reader thread (where the link callbacks fire);
// strips are serialized there, so the framebuffer / back-index state needs no
// lock. NO LVGL widgets are composited over the stream (a back button / status
// label would force LVGL to re-blend every frame — the draw cost we avoid):
// navigation back is a tap on the root, launch progress goes to the log.
class MirrorLauncher;

class ADBMirroringScreen : public Screen, public agent_link::LinkListener {
public:
    ADBMirroringScreen();
    ~ADBMirroringScreen() override;

    void build() override;
    void onExit() override;  // stop the launcher (closes the link) before destroy

    // agent_link::LinkListener — all on the adb reader thread.
    void on_link_hello(agent_link::Link* link, const agent_link::AgentInfo& info) override;
    void on_mirror_started(agent_link::Link* link, const agent_link::MirrorInfo& info) override;
    void on_video_strip(agent_link::Link* link, const agent_link::VideoStrip& strip) override;
    void on_link_close(agent_link::Link* link, adb::Error err) override;

private:
    // Decode one full-width strip straight into framebuffer `dst`, packed at row
    // strip.y (reader thread). Lazily creates the JPEG engine + DMA-capable input
    // buffer. Returns false on a decode error or a non-full-width strip.
    bool decode_strip(const agent_link::VideoStrip& strip, uint16_t* dst);
    void free_decoder();

    uint16_t* fb_[2] = {nullptr, nullptr};  // the two bsp framebuffers (not owned)
    int back_ = 0;                          // index the reader decodes into
    bool frame_ok_ = true;                  // all strips of the current frame decoded

    // FPS instrumentation (reader thread only): count presented frames / strips /
    // JPEG bytes and log a throughput line roughly once per second.
    int64_t stats_start_us_ = 0;  // start of the current ~1s window (0 = not started)
    uint32_t stats_frames_ = 0;   // frames presented in the window
    uint32_t stats_strips_ = 0;   // strips received in the window
    uint64_t stats_bytes_ = 0;    // JPEG bytes received in the window

    // JPEG decode (reader thread only) — the device/host-shared seam
    // (jpeg_fullrange_decode); strips land straight in the framebuffer, so there
    // is no decoded-pixel scratch, only the DMA-capable compressed input.
    jpeg_decoder_handle_t jpeg_ = nullptr;
    uint8_t* in_buf_ = nullptr;   // strip JPEG bytes, DMA-capable; grown lazily
    size_t in_cap_ = 0;

    std::shared_ptr<MirrorLauncher> launcher_;
};
