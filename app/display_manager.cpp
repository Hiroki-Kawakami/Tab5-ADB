#include "display_manager.hpp"

#include "adb_app.hpp"  // PANEL_W, PANEL_H
#include "bsp.h"

DisplayManager display_manager;

// Two BSP frame buffers drive LVGL in DIRECT mode: LVGL renders straight into
// the panel buffers and flush_cb hands the finished buffer to the BSP. The BSP
// (device drivers or the simulator's SDL backend) is the single hardware seam,
// so this setup is identical on both targets.
void DisplayManager::init() {
    bsp_pixel_format_t fmt = bsp_display_get_pixel_format();
    void  *fb0 = bsp_display_get_frame_buffer(0);
    void  *fb1 = bsp_display_get_frame_buffer(1);
    size_t bpp = bsp_pixel_format_bytes(fmt);

    auto disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_color_format(
        disp,
        fmt == BSP_PIXEL_FORMAT_RGB888 ? LV_COLOR_FORMAT_RGB888
                                       : LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, fb0, fb1, PANEL_W * PANEL_H * bpp,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, [](lv_display_t *disp, const lv_area_t *area,
                                     uint8_t *px_map) {
        // Map the rendered buffer back to its BSP frame-buffer index.
        int fb_index = (px_map == bsp_display_get_frame_buffer(1)) ? 1 : 0;
        bsp_display_flush(fb_index);
        lv_display_flush_ready(disp);
    });

    auto indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(indev, this);
    lv_indev_set_read_cb(indev, &DisplayManager::indev_read_cb);
}

void DisplayManager::indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    auto *self = static_cast<DisplayManager *>(lv_indev_get_user_data(indev));
    bsp_touch_point_t point;
    if (bsp_touch_read(&point, 1) > 0) {
        self->touch_pressed_ = true;
        self->touch_ = point;
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = point.x;
        data->point.y = point.y;
    } else {
        self->touch_pressed_ = false;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

bsp_pixel_format_t DisplayManager::format() const {
    return bsp_display_get_pixel_format();
}

void *DisplayManager::framebuffer(int index) const {
    return bsp_display_get_frame_buffer(index);
}

void DisplayManager::flush(int index) { bsp_display_flush(index); }

bool DisplayManager::touch_point(bsp_touch_point_t *out) const {
    if (!touch_pressed_) return false;
    if (out) *out = touch_;
    return true;
}
