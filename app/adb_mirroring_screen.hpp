#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include "adb_app.hpp"     // PANEL_W, PANEL_H
#include "agent_link.hpp"  // agent_link::Link, agent_link::VideoListener
#include "display_manager.hpp"   // DisplayManager::TouchListener
#include "jpeg_decode_enhanced.h"  // jpeg_enh_strip_decoder_handle_t (P4 HW on device, libjpeg on host)
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
// The agent lifecycle (jar push + app_process launch + HELLO) is owned by the
// app-global app::AgentClient, NOT this screen: on enter the screen calls
// ensure_connected() (showing a "Connecting…" label until ready, or reusing an
// already-live agent with no wait), then registers itself as the link's
// agent_link::VideoListener and calls Link::start_mirror(). On exit it sends
// Link::stop_mirror() and clears its video listener but LEAVES THE AGENT
// CONNECTED, so re-entering the screen resumes instantly.
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
// re-blend every frame — the draw cost we avoid): navigation back is the control
// strip's End button, launch progress goes to the log.
class ADBMirroringScreen : public Screen,
                           public agent_link::VideoListener,
                           public DisplayManager::TouchListener {
public:
    ADBMirroringScreen();
    ~ADBMirroringScreen() override;

    void build() override;
    void onEnter() override;  // ensure the agent is connected, then start the mirror
    void onExit() override;   // stop the mirror (link kept) + tear the decode down

    // agent_link::VideoListener — all fire on the adb reader thread.
    void on_mirror_started(agent_link::Link* link, const agent_link::MirrorInfo& info) override;
    void on_video_strip(agent_link::Link* link, const agent_link::VideoStrip& strip) override;
    void on_orientation(agent_link::Link* link, const agent_link::OrientationInfo& info) override;

    // DisplayManager::TouchListener — fires on the touch task thread. Detects the
    // bottom-left corner swipe that reveals a hidden control strip (see poll-free
    // note below).
    void on_touch(const bsp_touch_point_t* pts, int count) override;

private:
    // Once the agent is connected (or already live): swap the waiting label for the
    // overlay control strip, register as the link's video listener, and MIRROR_START.
    void start_mirror_ui();

    // Display mode (the overlay DispMode button), how the source maps onto the panel:
    //   Fit   — aspect-preserve inscribe, letterbox (agent scale=fit).
    //   Fill  — aspect-preserve cover, crop the overflow (agent scale=fill).
    //   Adapt — `wm size` the source to the panel aspect (so fit fills with no
    //           letterbox/crop), then agent scale=fit. Reset on leaving Adapt / exit.
    // Cycled by DispMode; the active mode is read off the image, so the icon is fixed.
    // LVGL-thread only.
    enum DispMode { kDispFit = 0, kDispFill = 1, kDispAdapt = 2 };
    int disp_mode_ = kDispFit;
    // Set from the `wm size` query at mirror start (LVGL thread):
    //   dispmode_show_  — false hides the DispMode button entirely (the source is
    //                     already panel-aspect 9:16/16:9, so fit == fill == adapt).
    //   adapt_allowed_  — false drops Adapt from the cycle (Fit<->Fill only): the
    //                     source runs a non-default `wm size` override we must not
    //                     clobber with Adapt's resize/reset.
    bool dispmode_show_ = true;
    bool adapt_allowed_ = true;
    // The MIRROR_START config for `mode` (Adapt and Fit both send scale=fit).
    agent_link::MirrorConfig mirror_config_for(int mode) const;
    // Switch to `mode`: reconfigure the live mirror (the agent restarts the stream on
    // a fresh MIRROR_START), running the `wm size` side effects for entering/leaving
    // Adapt. No-op if `mode` is already current.
    void apply_disp_mode(int mode);
    // Reconfigure the running mirror to `mode` (send a new MIRROR_START). LVGL thread.
    void restart_mirror(int mode);
    // Adapt enter: query `wm size`, set the panel-aspect override, then restart fit.
    // Adapt exit: `wm size reset`, then restart `mode`. Both chain over adb exec
    // completions marshalled back to the LVGL thread.
    void adapt_enter();
    void adapt_exit(int mode);
    // Query `wm size` at mirror start: set dispmode_show_ / adapt_allowed_ from the
    // source's current resolution (rebuilds the overlay if the DispMode button has to
    // be hidden). LVGL thread + an adb exec completion marshalled back.
    void query_disp_mode_availability();

    // The control overlay is an icon strip flush against one panel corner: a
    // vertical strip in portrait, a horizontal strip when the source device turns
    // landscape (the user physically rotates the Tab5; the overlay is PPA-rotated
    // to stay upright). It is keyed off the device's actual rotation
    // (Surface.ROTATION_* 0..3, from the agent's ORIENTATION event) so it always
    // lands at the **viewer's** bottom-left: ROTATION_90 vs _270 put that corner at
    // opposite physical panel corners and need opposite PPA angles. Hidden/shown
    // without a timeout: the in-strip Hide button hides it; a swipe out of that
    // corner (the L-shaped in_corner hot zone) reveals it again.
    static bool rot_landscape(uint8_t rot) { return (rot & 1) != 0; }
    // A landscape app is viewed by turning the Tab5 the opposite way from the naive
    // guess (verified on real HW), so the overlay's PPA angle + anchor corner use
    // this corrected rotation: it swaps ROTATION_90 <-> _270 and leaves portrait
    // (0/180) alone, keeping the overlay aligned with the video (both flip
    // together). Flip back to `rot` if a later device shows the other handedness.
    static uint8_t view_rot(uint8_t rot) { return (4 - rot) & 3; }
    // The panel corner the strip anchors to (= the viewer's bottom-left) for device
    // rotation `rot`: view_rot 0->bottom-left, 1->bottom-right, 2->top-right,
    // 3->top-left. Used by both apply_overlay (footprint) and in_corner (hot zone).
    static void anchor_corner(uint8_t rot, bool* right, bool* bottom) {
        uint8_t g = view_rot(rot);
        *right = (g == 1 || g == 2);
        *bottom = (g == 0 || g == 1);
    }
    // Build (first=true: clear framebuffers / fresh entry) or rebuild (first=false,
    // a rotation change keeps visibility) the overlay for device rotation `rot`,
    // then populate its buttons.
    void apply_overlay(uint8_t rot, bool first);
    // Lay the control buttons + group separators onto the overlay screen `scr` in
    // the spec'd order — vertical (portrait) or horizontal (landscape). The content
    // is always built "upright"; the footprint's PPA rotation orients it.
    void build_overlay_buttons(lv_obj_t* scr, bool landscape);
    // True when (px,py) is inside the reveal hot zone — an L hugging the anchor
    // corner for `rot` (two narrow bands along the corner's two edges, see kEdge*).
    static bool in_corner(int px, int py, uint8_t rot);
    // True when (px,py) is inside the overlay strip footprint for `rot`. Used to
    // keep touches over a visible strip out of passthrough (LVGL handles them).
    static bool in_overlay_footprint(int px, int py, uint8_t rot);

    // --- overlay control strip geometry (all adjustable). The strip hugs the
    // panel corner (no margin); one button size for both orientations. ---
    // Reveal hot zone: an L hugging the anchor corner = two NARROW bands, one
    // along each edge meeting at the corner, so it steals little area from touch
    // passthrough. A press starting in the L that drags >= kSwipeThresh reveals
    // the hidden strip.
    static constexpr int kEdgeThick = 24;    // band thickness along each edge [px]
    static constexpr int kEdgeReach = 160;   // band length from the corner [px]
    static constexpr int kSwipeThresh = 28;  // min drag from the corner to reveal
    static constexpr int kBtn = 56;          // button (square) side
    static constexpr int kPad = 12;          // strip inner padding
    static constexpr int kGap = 8;           // gap between adjacent items
    static constexpr int kSep = 2;           // group separator line thickness
    // The strip holds 10 buttons + 3 separators (13 items, 12 gaps). Long axis =
    // `len`, thickness across = `cross`; the panel footprint is always cross x len
    // (the landscape strip is PPA-rotated, so its long axis still runs up panel y).
    static constexpr int kStripCross = kBtn + 2 * kPad;
    static constexpr int kStripLen = 10 * kBtn + 3 * kSep + 12 * kGap + 2 * kPad;
    // The smallest strip is 16px tall, so a panel holds at most this many strips.
    static constexpr int kMaxStrips = PANEL_H / 16;
    // Frame slots in flight: the consumer holds the decoding slot AND retains the
    // last decoded frame (to re-decode and erase the strip when the overlay is
    // hidden over static video), the producer holds the filling one, and ready_q_
    // holds at most one — plus headroom so a steady stream never drops spuriously.
    static constexpr int kSlots = 5;

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
    // error or a non-full-width strip. `dst` is byte-addressed (the panel format —
    // RGB565 or RGB888 — is resolved from the DisplayManager at decode time).
    bool decode_one(uint8_t* jpeg, uint32_t len, uint16_t y, uint16_t h,
                    uint8_t* dst);
    void free_decoder();

    // The bsp framebuffers (not owned), used as a TRIPLE buffer: the decode task
    // draws into fb_[back_], flushes it (it becomes the front/displayed buffer),
    // then advances back_ to the next buffer in the rotation. With three buffers
    // the buffer drawn into was last displayed two frames ago, so the decode task
    // never waits on the panel's scan-out/vsync sync before reusing one.
    static constexpr int kFbCount = 3;
    uint8_t* fb_[kFbCount] = {nullptr, nullptr, nullptr};  // byte-addressed (565 or 888)
    int back_ = 0;     // index the decode task draws into next
    int front_ = -1;   // index currently displayed (last flushed; -1 = none yet)

    uint8_t cur_rot_ = 0;  // LVGL thread: device rotation the overlay was built for

    // --- touch passthrough (§4.7) ---
    // Touch-control mode: when on (the default), touches over the mirror (outside
    // a visible overlay strip / the reveal corner) are injected to the source
    // device as per-pointer MotionEvents. Toggled by the overlay OpMode button
    // (LVGL thread); read on the touch task thread.
    std::atomic<bool> passthrough_{true};

    // Active touch points, keyed by the controller track id, tracked across
    // touch-task samples so on_touch can emit per-pointer DOWN/MOVE/UP. Each
    // pointer's role is decided at DOWN and kept for its lifetime: Pass = injected
    // to the device, Reveal = a corner-swipe candidate, Ignore = handled by the
    // overlay / dropped. Guarded by pass_mtx_ so onExit (LVGL thread) can
    // release_all_pointers() any still-down Pass pointers.
    enum class PtKind : uint8_t { Pass, Reveal, Ignore };
    struct ActivePtr {
        bool used = false;
        int id = 0;
        PtKind kind = PtKind::Ignore;
        uint16_t x = 0, y = 0;     // last position (for the UP coords)
        int rx0 = 0, ry0 = 0;      // reveal-swipe start (Reveal only)
    };
    static constexpr int kMaxPass = 10;
    ActivePtr pass_[kMaxPass];
    std::mutex pass_mtx_;
    // UP every still-down Pass pointer and clear the table (any thread). Called on
    // exit / when passthrough is turned off, so the source sees no stuck finger.
    void release_all_pointers();

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
    // (jpeg_decode_enhanced, whole-frame full-range decode); strips land straight
    // in the framebuffer, so there is no decoded-pixel scratch and the slot
    // buffer is the compressed input.
    jpeg_enh_strip_decoder_handle_t jpeg_ = nullptr;

    // LVGL thread only. The "Connecting…" label shown until the agent is ready;
    // overlay_active_ tracks whether we entered DM overlay mode (so onExit only
    // exits it once we actually started mirroring).
    void* waiting_label_ = nullptr;  // lv_obj_t*
    bool overlay_active_ = false;
};
