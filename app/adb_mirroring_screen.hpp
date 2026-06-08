#pragma once
#include <atomic>
#include <memory>
#include <string>

#include "agent_link.hpp"  // agent_link::Link, agent_link::LinkListener
#include "driver/jpeg_decode.h"  // jpeg_decoder_handle_t (IDF on device, shim on host)
#include "screen.hpp"

// Screen-mirror viewer. Launches tab5adb-agent on the connected device (pushes
// the embedded jar, app_process), opens an agent_link::Link, sends MIRROR_START,
// and renders the incoming JPEG strip stream into a full-screen LVGL canvas.
//
// The screen IS the agent_link::LinkListener for the UI-facing callbacks
// (hello/mirror-started/strip/close); the multi-step launch + the retry until the
// agent is listening run on a private worker task owned by MirrorLauncher (a
// file-local helper). Link callbacks fire on the adb reader thread, so this
// decodes each strip there (into fb_) and marshals only the canvas invalidate to
// the LVGL thread with lv_async_call — the same threading contract as the shell /
// file-browser screens.
//
// v1 is deliberately bare: the screen is ONLY the stream canvas — NO LVGL
// widgets composited over it (a back button / status label on top would force
// LVGL to re-blend them on every frame, which is exactly the draw-cost we are
// avoiding for now). Navigation back is a tap on the canvas (an input handler, no
// extra draw layer); launch progress goes to the log. A single canvas buffer
// (no double-buffering) means a frame can tear if the reader writes while LVGL
// blits — acceptable for the first "just show the stream" milestone.
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
    // Decode one strip into fb_ at (x,y) (reader thread). Lazily creates the JPEG
    // engine + scratch buffers on first use. Returns false on a decode error.
    bool decode_strip(const agent_link::VideoStrip& strip);
    void free_decoder();

    lv_obj_t* canvas_ = nullptr;

    uint16_t* fb_ = nullptr;  // panel-sized RGB565 canvas buffer (PSRAM)

    // JPEG decode (reader thread only). The engine + DMA-capable scratch buffers
    // are the device/host-shared decode seam (jpeg_fullrange_decode).
    jpeg_decoder_handle_t jpeg_ = nullptr;
    uint8_t* in_buf_ = nullptr;   // strip JPEG bytes, DMA-capable; grown lazily
    size_t in_cap_ = 0;
    uint8_t* out_buf_ = nullptr;  // decoded RGB565, DMA-capable; panel-sized
    size_t out_cap_ = 0;

    std::shared_ptr<MirrorLauncher> launcher_;
    std::atomic<bool> invalidate_pending_{false};  // coalesce canvas invalidates
};
