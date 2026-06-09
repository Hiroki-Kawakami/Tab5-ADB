#pragma once
#include "bsp_types.h"  // bsp_pixel_format_t, bsp_touch_point_t
#include "lvgl.hpp"

// Central owner of the panel framebuffers, the LVGL display, and the touch
// indev. App code drives the screen through this instead of calling
// bsp_display_* directly, so the normal full-screen LVGL path and (in a later
// step) the mirror's overlay-compositing path share one seam.
//
// The panel pixel format is fixed at boot (chosen in bsp_init); changing it
// requires a restart. LVGL always renders RGB565; when the panel is driven
// RGB888 the format is converted on the way to the framebuffer (added with the
// 888 path in a later step).
class DisplayManager {
public:
    // Create the main LVGL display + the touch indev over the BSP framebuffers
    // and start in normal mode. Call once from adb_app(), after bsp_init().
    void init();

    // --- BSP framebuffer wrapper (use these, not bsp_display_* directly) ---
    bsp_pixel_format_t format() const;
    void *framebuffer(int index) const;
    // Present framebuffer `index`. (Overlay compositing is layered onto this in a
    // later step; in normal mode it is a thin bsp_display_flush.)
    void  flush(int index);

    // Latest raw touch in PANEL coordinates, cached on every indev read (LVGL
    // thread). Returns true and fills *out while pressed, false while released.
    // Poll from the LVGL thread (e.g. an lv_timer). Lets a screen observe touches
    // that land outside any LVGL widget — used by the mirror to control overlay
    // show/hide and, later, to forward input to the phone.
    bool touch_point(bsp_touch_point_t *out) const;

private:
    static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

    bool              touch_pressed_ = false;
    bsp_touch_point_t touch_ = {};
};

extern DisplayManager display_manager;
