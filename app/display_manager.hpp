#pragma once
#include "bsp_types.h"  // bsp_pixel_format_t, bsp_touch_point_t
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

    // Latest raw touch in PANEL coordinates, cached on every indev read (LVGL
    // thread). Returns true and fills *out while pressed, false while released.
    // Poll from the LVGL thread (e.g. an lv_timer). Lets a screen observe touches
    // that land outside any LVGL widget — used by the mirror to control overlay
    // show/hide and, later, to forward input to the phone.
    bool touch_point(bsp_touch_point_t *out) const;

    // --- overlay mode (call from the LVGL thread, e.g. onEnter/onExit) ---
    struct OverlayConfig {
        lv_area_t rect;                 // panel-space destination footprint (inclusive)
        int   rotation = 0;             // 0/90/180/270, CCW (PPA SRM convention)
        float scale = 1.0f;             // content -> footprint scale
        bool  clear_framebuffers = false;  // black both framebuffers (+flush) on enter
    };
    // Enter overlay mode: create the overlay LVGL display sized to the (unrotated,
    // unscaled) content, route the indev to it, and return its active screen for
    // the caller to build the bar onto. The overlay starts hidden.
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
    // PPA-composite the overlay buffer into framebuffer `index` at the footprint
    // rect (rotate/scale/format-convert). Caller holds the LVGL lock.
    void compose_overlay(int index);
    // Map a panel-space point into overlay content coordinates (inverse of the
    // footprint offset + PPA rotate/scale). Clamped to the content rect.
    void panel_to_content(int px, int py, int *lx, int *ly) const;

    lv_display_t *main_disp_ = nullptr;
    lv_indev_t   *indev_ = nullptr;

    bool              touch_pressed_ = false;
    bsp_touch_point_t touch_ = {};

    Mode          mode_ = Mode::Normal;
    lv_display_t *overlay_disp_ = nullptr;
    uint8_t      *overlay_buf_ = nullptr;   // RGB565, content_w_*content_h_*2 (PSRAM)
    int           content_w_ = 0, content_h_ = 0;
    lv_area_t     overlay_rect_ = {};
    int           overlay_rotation_ = 0;
    float         overlay_scale_ = 1.0f;
    bool          overlay_visible_ = false;
    void         *ppa_srm_ = nullptr;       // ppa_client_handle_t (lazy)
};

extern DisplayManager display_manager;
