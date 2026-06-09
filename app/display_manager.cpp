#include "display_manager.hpp"

#include <cmath>
#include <cstring>

#include "adb_app.hpp"  // PANEL_W, PANEL_H
#include "bsp.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"

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

    main_disp_ = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_color_format(
        main_disp_,
        fmt == BSP_PIXEL_FORMAT_RGB888 ? LV_COLOR_FORMAT_RGB888
                                       : LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(main_disp_, fb0, fb1, PANEL_W * PANEL_H * bpp,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(main_disp_, [](lv_display_t *disp, const lv_area_t *area,
                                           uint8_t *px_map) {
        // Map the rendered buffer back to its BSP frame-buffer index.
        int fb_index = (px_map == bsp_display_get_frame_buffer(1)) ? 1 : 0;
        bsp_display_flush(fb_index);
        lv_display_flush_ready(disp);
    });

    indev_ = lv_indev_create();
    lv_indev_set_type(indev_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(indev_, this);
    lv_indev_set_read_cb(indev_, &DisplayManager::indev_read_cb);
}

void DisplayManager::indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    auto *self = static_cast<DisplayManager *>(lv_indev_get_user_data(indev));
    bsp_touch_point_t point;
    bool pressed = bsp_touch_read(&point, 1) > 0;
    self->touch_pressed_ = pressed;
    if (pressed) self->touch_ = point;  // raw panel coords, cached for touch_point()

    if (!pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (self->mode_ == Mode::Overlay) {
        // The indev is routed to the overlay display in overlay mode. Only feed it
        // when the bar is visible and the touch lands inside the footprint — taps
        // elsewhere (and while hidden) are the screen's to handle via touch_point.
        bool inside = self->overlay_visible_ &&
                      point.x >= self->overlay_rect_.x1 && point.x <= self->overlay_rect_.x2 &&
                      point.y >= self->overlay_rect_.y1 && point.y <= self->overlay_rect_.y2;
        if (!inside) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        int lx, ly;
        self->panel_to_content(point.x, point.y, &lx, &ly);
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = lx;
        data->point.y = ly;
    } else {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = point.x;
        data->point.y = point.y;
    }
}

bsp_pixel_format_t DisplayManager::format() const {
    return bsp_display_get_pixel_format();
}

void *DisplayManager::framebuffer(int index) const {
    return bsp_display_get_frame_buffer(index);
}

void DisplayManager::flush(int index) {
    if (mode_ == Mode::Overlay && overlay_visible_) {
        // The compose reads the overlay buffer the LVGL renderer writes, so take
        // the LVGL lock (this runs on the screen's decode/worker thread, not the
        // LVGL thread). PPA is blocking, so the lock is held only briefly.
        lv_lock();
        compose_overlay(index);
        lv_unlock();
    }
    bsp_display_flush(index);
}

bool DisplayManager::touch_point(bsp_touch_point_t *out) const {
    if (!touch_pressed_) return false;
    if (out) *out = touch_;
    return true;
}

// MARK: Overlay mode

lv_obj_t *DisplayManager::enter_overlay(const OverlayConfig &cfg) {
    overlay_rect_ = cfg.rect;
    overlay_rotation_ = cfg.rotation;
    overlay_scale_ = cfg.scale > 0 ? cfg.scale : 1.0f;
    overlay_visible_ = false;

    int rect_w = lv_area_get_width(&overlay_rect_);
    int rect_h = lv_area_get_height(&overlay_rect_);
    // Content is the unrotated, unscaled footprint. A 90/270 rotation swaps the
    // footprint's width/height relative to the content.
    bool swap = (overlay_rotation_ == 90 || overlay_rotation_ == 270);
    content_w_ = (int)lroundf((swap ? rect_h : rect_w) / overlay_scale_);
    content_h_ = (int)lroundf((swap ? rect_w : rect_h) / overlay_scale_);

    size_t buf_bytes = (size_t)content_w_ * content_h_ * 2;  // RGB565
    overlay_buf_ = static_cast<uint8_t *>(
        heap_caps_aligned_alloc(64, buf_bytes, MALLOC_CAP_SPIRAM));
    if (overlay_buf_) std::memset(overlay_buf_, 0, buf_bytes);

    overlay_disp_ = lv_display_create(content_w_, content_h_);
    lv_display_set_color_format(overlay_disp_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(overlay_disp_, overlay_buf_, nullptr, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    // The compositor reads overlay_buf_ at flush time, so the flush_cb only acks.
    lv_display_set_flush_cb(overlay_disp_, [](lv_display_t *disp, const lv_area_t *,
                                              uint8_t *) {
        lv_display_flush_ready(disp);
    });
    // Keep the main display as LVGL's default so ScreenManager (lv_screen_load,
    // theme) keeps targeting it; the overlay screen is reached explicitly.
    lv_display_set_default(main_disp_);

    // Route touch to the overlay display (read_cb maps panel -> content coords).
    lv_indev_set_display(indev_, overlay_disp_);
    mode_ = Mode::Overlay;

    if (cfg.clear_framebuffers) {
        size_t fb_bytes = (size_t)PANEL_W * PANEL_H * bsp_pixel_format_bytes(format());
        for (int i = 0; i < 2; ++i) {
            void *fb = bsp_display_get_frame_buffer(i);
            if (fb) std::memset(fb, 0, fb_bytes);
            bsp_display_flush(i);
        }
    }

    return lv_display_get_screen_active(overlay_disp_);
}

void DisplayManager::exit_overlay() {
    if (mode_ != Mode::Overlay) return;
    mode_ = Mode::Normal;
    overlay_visible_ = false;
    lv_indev_set_display(indev_, main_disp_);
    if (overlay_disp_) { lv_display_delete(overlay_disp_); overlay_disp_ = nullptr; }
    if (overlay_buf_) { heap_caps_free(overlay_buf_); overlay_buf_ = nullptr; }
}

void DisplayManager::set_overlay_visible(bool visible) { overlay_visible_ = visible; }

void DisplayManager::compose_overlay(int index) {
    if (!overlay_buf_) return;
    if (!ppa_srm_) {
        ppa_client_config_t cc = {};
        cc.oper_type = PPA_OPERATION_SRM;
        ppa_client_handle_t client = nullptr;
        if (ppa_register_client(&cc, &client) != ESP_OK) return;
        ppa_srm_ = client;
    }
    void *fb = bsp_display_get_frame_buffer(index);
    if (!fb) return;
    bool rgb888 = (format() == BSP_PIXEL_FORMAT_RGB888);

    ppa_srm_oper_config_t op = {};
    op.in.buffer = overlay_buf_;
    op.in.pic_w = content_w_;
    op.in.pic_h = content_h_;
    op.in.block_w = content_w_;
    op.in.block_h = content_h_;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = fb;
    op.out.buffer_size = (uint32_t)PANEL_W * PANEL_H * bsp_pixel_format_bytes(format());
    op.out.pic_w = PANEL_W;
    op.out.pic_h = PANEL_H;
    op.out.block_offset_x = overlay_rect_.x1;
    op.out.block_offset_y = overlay_rect_.y1;
    op.out.srm_cm = rgb888 ? PPA_SRM_COLOR_MODE_RGB888 : PPA_SRM_COLOR_MODE_RGB565;
    switch (overlay_rotation_) {
        case 90:  op.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;  break;
        case 180: op.rotation_angle = PPA_SRM_ROTATION_ANGLE_180; break;
        case 270: op.rotation_angle = PPA_SRM_ROTATION_ANGLE_270; break;
        default:  op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;   break;
    }
    op.scale_x = overlay_scale_;
    op.scale_y = overlay_scale_;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    ppa_do_scale_rotate_mirror(static_cast<ppa_client_handle_t>(ppa_srm_), &op);
}

void DisplayManager::panel_to_content(int px, int py, int *lx, int *ly) const {
    // Panel point relative to the footprint origin.
    float ox = px - overlay_rect_.x1;
    float oy = py - overlay_rect_.y1;
    float s = overlay_scale_;
    float cw = content_w_, ch = content_h_;
    float x = 0, y = 0;
    // Inverse of the PPA scale->rotate(CCW) forward map (matches the sim shim /
    // device HW). Forward: rot90 -> (ox,oy)=(ly*s, (cw*s-1-lx*s)); etc.
    switch (overlay_rotation_) {
        case 90:  x = (cw * s - 1 - oy) / s; y = ox / s;             break;
        case 180: x = (cw * s - 1 - ox) / s; y = (ch * s - 1 - oy) / s; break;
        case 270: x = oy / s;                y = (ch * s - 1 - ox) / s; break;
        default:  x = ox / s;                y = oy / s;             break;
    }
    int xi = (int)lroundf(x), yi = (int)lroundf(y);
    if (xi < 0) xi = 0; else if (xi > content_w_ - 1) xi = content_w_ - 1;
    if (yi < 0) yi = 0; else if (yi > content_h_ - 1) yi = content_h_ - 1;
    *lx = xi;
    *ly = yi;
}
