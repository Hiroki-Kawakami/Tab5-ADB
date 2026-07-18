#pragma once
#include <memory>  // std::weak_ptr (touch listener)

#include "bsp_types.h"  // bsp_pixel_format_t, bsp_touch_point_t
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"  // SemaphoreHandle_t (overlay compositor lock)
#include "lvgl.hpp"

// Central owner of the panel framebuffers, the LVGL display, the touch indev, and
// the mirror overlay compositor. App code drives the screen through this instead
// of calling bsp_display_* directly, so the normal full-screen LVGL path and the
// mirror's overlay-compositing path share one seam.
//
// The panel pixel format is fixed at boot (chosen in bsp_init); changing it
// requires a restart. LVGL always renders RGB565; when the panel is driven
// RGB888 the format is converted on the way to the framebuffer.
//
// Two modes:
//  - Normal: the main LVGL display drives the panel framebuffers directly (full-
//    screen UI). flush() is a thin bsp_display_flush.
//  - Overlay: a separate small LVGL display renders an opaque bar into its own
//    RGB565 buffer; the screen (e.g. the mirror) owns the framebuffers and writes
//    them (JPEG decode), then calls flush(index) — which, while the overlay is
//    visible, PPA-composites the bar into the framebuffer (rotate/scale/format-
//    convert) before presenting. The screen decides which buffer / whether to
//    swap, so it controls frame cadence (got-frame → swap; timeout → recomposite
//    the bar on the displayed buffer in place).
class DisplayManager {
public:
    // Create the main LVGL display + the touch indev over the BSP framebuffers
    // and start in normal mode. Call once from adb_app(), after bsp_init().
    void init();

    // --- BSP framebuffer wrapper (use these, not bsp_display_* directly) ---
    bsp_pixel_format_t format() const;
    void *framebuffer(int index) const;
    // Present framebuffer `index`. In overlay mode with the overlay visible, the
    // overlay bar is PPA-composited into `index` first; in normal mode this is a
    // thin bsp_display_flush.
    void  flush(int index);

    // --- raw touch observation (pushed, multi-touch) ---
    // Touch arrives from the BSP dispatch task, decoupled from the LVGL render
    // loop, so a screen can observe touches that land outside any LVGL widget —
    // the mirror uses this for the overlay show/hide swipe and, later, to forward
    // multi-touch to the phone. A TouchListener is pushed every sample;
    // `pts[0..count)` are raw PANEL coordinates and count==0 means all fingers
    // lifted. on_touch fires on the BSP dispatch task (NOT the LVGL thread) — the
    // listener marshals to LVGL itself if it touches widgets.
    struct TouchListener {
        virtual void on_touch(const bsp_touch_point_t *pts, int count) = 0;
        virtual ~TouchListener() = default;
    };
    // Register (empty weak_ptr clears) the touch listener. Held weakly — drop your
    // shared_ptr to detach (Shell/Sync-style). Settable from any thread.
    void set_touch_listener(std::weak_ptr<TouchListener> listener);
    // Mask the in-progress press from the LVGL indev until the finger lifts. The
    // mirror's swipe-to-reveal calls this so the gesture that reveals the overlay
    // isn't also delivered as a press to the freshly-shown overlay buttons. Call
    // BEFORE set_overlay_visible(true) to close the visible-without-mask window.
    void consume_overlay_touch();

    // --- overlay mode (call from the LVGL thread, e.g. onEnter/onExit) ---
    struct OverlayConfig {
        lv_area_t rect;                 // panel-space destination footprint (inclusive)
        int   rotation = 0;             // 0/90/180/270, CCW (PPA SRM convention)
        float scale = 1.0f;             // content -> footprint scale
        bool  clear_framebuffers = false;  // black both framebuffers (+flush) on enter
    };
    // Enter (or, if already in overlay mode, rebuild) the overlay: create the
    // overlay LVGL display sized to the (unrotated, unscaled) content, route the
    // indev to it, and return its active screen for the caller to build the strip
    // onto. Re-entrant — call it again with a new footprint / rotation to flip the
    // layout (e.g. mirror portrait <-> landscape) while still in overlay mode; it
    // tears the old overlay display + buffer down and builds the new one. The
    // overlay starts hidden each time (the caller re-asserts set_overlay_visible).
    // Call from the LVGL thread. The buffer swap is serialized against the
    // compositor (overlay_mtx_), so a concurrent compose on the decode thread can't
    // read a freed buffer.
    lv_obj_t *enter_overlay(const OverlayConfig &cfg);
    // Leave overlay mode: restore the indev to the main display and destroy the
    // overlay display/buffer. The caller must have stopped writing/flushing the
    // framebuffers first; the underlying screen's re-render reclaims the panel.
    void exit_overlay();
    // Toggle whether flush() composites the overlay bar (the caller's policy).
    void set_overlay_visible(bool visible);
    bool overlay_visible() const { return overlay_visible_; }

private:
    enum class Mode { Normal, Overlay };

    static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data);
    static void touch_event_cb(const bsp_touch_point_t *points, int count, void *arg);
    // Main display flush: presents the panel framebuffer in normal mode; in
    // overlay mode it only acks (the main display renders into a scratch buffer
    // and the mirror owns the panel framebuffers).
    static void main_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
    // PPA-composite the overlay buffer into framebuffer `index` at the footprint
    // rect (rotate/scale/format-convert). Caller holds the LVGL lock.
    void compose_overlay(int index);
    // Map a panel-space point into overlay content coordinates (inverse of the
    // footprint offset + PPA rotate/scale). Clamped to the content rect.
    void panel_to_content(int px, int py, int *lx, int *ly) const;

    lv_display_t *main_disp_ = nullptr;
    lv_indev_t   *indev_ = nullptr;

    // LVGL indev feed (single tap = touch id 0): produced by the BSP callback,
    // consumed by indev_read_cb — both under touch_mtx_. lvgl_suppress_ masks the
    // press until the finger lifts (consume_overlay_touch); the task clears it on
    // the first all-lifted sample.
    static constexpr int kMaxTouch = 5;
    bool              lvgl_pressed_ = false;
    bsp_touch_point_t lvgl_pt_ = {};
    bool              lvgl_suppress_ = false;
    std::weak_ptr<TouchListener> touch_listener_;
    SemaphoreHandle_t touch_mtx_ = nullptr;   // guards the indev feed + listener

    Mode          mode_ = Mode::Normal;
    lv_display_t *overlay_disp_ = nullptr;
    uint8_t      *overlay_buf_ = nullptr;   // RGB565, content_w_*content_h_*2 (PSRAM)
    int           content_w_ = 0, content_h_ = 0;
    lv_area_t     overlay_rect_ = {};
    int           overlay_rotation_ = 0;
    float         overlay_scale_ = 1.0f;
    bool          overlay_visible_ = false;
    // Serializes the overlay geometry + buffer (compose_overlay reads them on the
    // decode thread; enter/reconfigure/exit_overlay write them on the LVGL thread).
    // A FreeRTOS mutex (priority inheritance), created in init(). A leaf lock —
    // compose never takes lv_lock, so it can't deadlock the teardown join (see
    // flush()).
    SemaphoreHandle_t overlay_mtx_ = nullptr;
    void         *ppa_srm_ = nullptr;       // ppa_client_handle_t (lazy)
    uint8_t      *main_scratch_ = nullptr;  // full-screen buffer the main display
                                            // renders into while in overlay mode
                                            // (keeps it off the panel framebuffers)
};

extern DisplayManager display_manager;
