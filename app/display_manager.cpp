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
    lv_display_set_flush_cb(main_disp_, &DisplayManager::main_flush_cb);

    indev_ = lv_indev_create();
    lv_indev_set_type(indev_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(indev_, this);
    lv_indev_set_read_cb(indev_, &DisplayManager::indev_read_cb);

    overlay_mtx_ = xSemaphoreCreateMutex();
    touch_mtx_ = xSemaphoreCreateMutex();

    // Sample touch on a dedicated task so the hardware read is decoupled from the
    // LVGL render loop (panel refresh / JPEG decode never delay input) and idles
    // while untouched. indev_read_cb just returns the feed this task produces.
    touch_stop_ = false;
    xTaskCreate(&DisplayManager::touch_trampoline, "touch", 4096, this, 5, &touch_task_);
}

void DisplayManager::main_flush_cb(lv_display_t *disp, const lv_area_t * /*area*/,
                                   uint8_t *px_map) {
    // In overlay mode the main display renders into a scratch buffer and the mirror
    // owns the panel framebuffers, so don't present anything here.
    if (display_manager.mode_ == Mode::Overlay) {
        lv_display_flush_ready(disp);
        return;
    }
    int fb_index = (px_map == bsp_display_get_frame_buffer(1)) ? 1 : 0;
    bsp_display_flush(fb_index);
    lv_display_flush_ready(disp);
}

void DisplayManager::indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    auto *self = static_cast<DisplayManager *>(lv_indev_get_user_data(indev));
    // The hardware read happens on the touch task; here we only consume the cached
    // single-tap feed. lvgl_suppress_ masks the press until the finger lifts (the
    // swipe-to-reveal gesture), so check it together with the press state.
    bool pressed;
    bsp_touch_point_t point;
    xSemaphoreTake(self->touch_mtx_, portMAX_DELAY);
    pressed = self->lvgl_pressed_ && !self->lvgl_suppress_;
    point = self->lvgl_pt_;
    xSemaphoreGive(self->touch_mtx_);

    if (self->mode_ == Mode::Overlay) {
        // The indev is routed to the small overlay display, so the reported point
        // must stay within its (content) resolution — LVGL retains the last point
        // across reads and warns if a stale panel-space Y exceeds the display
        // height. Feed a press only when the bar is visible and the touch lands
        // inside the footprint; otherwise report RELEASED but still clamp the point
        // into content range (preserving the press location so a Back-button click
        // — press inside, release on lift — still registers). Taps elsewhere are
        // the screen's to handle via the TouchListener.
        if (pressed) {
            int lx, ly;
            self->panel_to_content(point.x, point.y, &lx, &ly);  // clamped to content
            data->point.x = lx;
            data->point.y = ly;
            bool inside = self->overlay_visible_ &&
                          point.x >= self->overlay_rect_.x1 && point.x <= self->overlay_rect_.x2 &&
                          point.y >= self->overlay_rect_.y1 && point.y <= self->overlay_rect_.y2;
            data->state = inside ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        } else {
            // Clamp the retained point into range (covers a stale panel point left
            // from the normal->overlay transition); keep it otherwise so a button
            // release lands where the press was.
            if (data->point.x > self->content_w_ - 1) data->point.x = self->content_w_ - 1;
            if (data->point.y > self->content_h_ - 1) data->point.y = self->content_h_ - 1;
            if (data->point.x < 0) data->point.x = 0;
            if (data->point.y < 0) data->point.y = 0;
            data->state = LV_INDEV_STATE_RELEASED;
        }
        return;
    }
    // Normal mode: report panel coordinates on press, leave the point untouched on
    // release (LVGL keeps the press point so clicks register on the main display).
    if (pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = point.x;
        data->point.y = point.y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
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
        // Composite the overlay bar into the framebuffer. This runs on the screen's
        // decode/worker thread and reads the overlay buffer the LVGL renderer
        // writes — deliberately WITHOUT taking the LVGL lock: the bar is static
        // (re-rendered only on a Back-button press), so a concurrent read is at
        // worst a one-frame cosmetic tear, never a crash. Taking lv_lock here would
        // deadlock teardown — onExit() joins this thread from inside lv_timer_handler
        // (which holds lv_lock), so this thread must never block on lv_lock.
        compose_overlay(index);
    }
    bsp_display_flush(index);
}

void DisplayManager::set_touch_listener(std::weak_ptr<TouchListener> listener) {
    xSemaphoreTake(touch_mtx_, portMAX_DELAY);
    touch_listener_ = std::move(listener);
    xSemaphoreGive(touch_mtx_);
}

void DisplayManager::set_touch_poll_hz(int hz) {
    if (hz > 0) touch_poll_hz_ = hz;
}

void DisplayManager::consume_overlay_touch() {
    xSemaphoreTake(touch_mtx_, portMAX_DELAY);
    lvgl_suppress_ = true;
    xSemaphoreGive(touch_mtx_);
}

void DisplayManager::touch_trampoline(void *arg) {
    static_cast<DisplayManager *>(arg)->touch_loop();
}

void DisplayManager::touch_loop() {
    // Block on the controller INT (bsp_touch_wait_interrupt) while untouched, then
    // poll at touch_poll_hz_; after kIdleStop consecutive empty reads stop polling
    // and wait for the next INT again. The first empty read still publishes the
    // RELEASED state so a tap's release reaches LVGL before we idle.
    constexpr int kIdleStop = 3;
    bsp_touch_point_t pts[kMaxTouch];
    while (!touch_stop_) {
        bsp_touch_wait_interrupt();
        int empty = 0;
        while (!touch_stop_) {
            int n = bsp_touch_read(pts, kMaxTouch);
            if (n < 0) n = 0;
            else if (n > kMaxTouch) n = kMaxTouch;

            std::weak_ptr<TouchListener> lw;
            xSemaphoreTake(touch_mtx_, portMAX_DELAY);
            lvgl_pressed_ = (n > 0);            // LVGL indev = single tap (id 0)
            if (n > 0) lvgl_pt_ = pts[0];
            if (n == 0) lvgl_suppress_ = false;  // gesture ended -> unmask
            lw = touch_listener_;
            xSemaphoreGive(touch_mtx_);

            // Push every contemporaneous point (multi-touch) to the listener, on
            // this thread, outside the lock so it can call back into DisplayManager.
            if (auto l = lw.lock()) l->on_touch(pts, n);

            if (n == 0) {
                if (++empty >= kIdleStop) break;
            } else {
                empty = 0;
            }
            int hz = touch_poll_hz_ > 0 ? touch_poll_hz_ : 60;
            vTaskDelay(pdMS_TO_TICKS(1000 / hz));
        }
    }
    vTaskDelete(nullptr);
}

// MARK: Overlay mode

lv_obj_t *DisplayManager::enter_overlay(const OverlayConfig &cfg) {
    // Re-entrant: called both to first enter overlay mode and to rebuild the
    // overlay for a new footprint / rotation (e.g. the mirror flipping portrait
    // <-> landscape). Drop any existing overlay display first (LVGL thread → no
    // concurrent render); its widgets go with it, and the caller rebuilds onto the
    // returned screen.
    if (overlay_disp_) { lv_display_delete(overlay_disp_); overlay_disp_ = nullptr; }

    float scale = cfg.scale > 0 ? cfg.scale : 1.0f;
    int rect_w = lv_area_get_width(&cfg.rect);
    int rect_h = lv_area_get_height(&cfg.rect);
    // Content is the unrotated, unscaled footprint. A 90/270 rotation swaps the
    // footprint's width/height relative to the content.
    bool swap = (cfg.rotation == 90 || cfg.rotation == 270);
    int new_cw = (int)lroundf((swap ? rect_h : rect_w) / scale);
    int new_ch = (int)lroundf((swap ? rect_w : rect_h) / scale);
    size_t buf_bytes = (size_t)new_cw * new_ch * 2;  // RGB565

    // Swap geometry + buffer under the compositor lock so that, on a re-entry while
    // still streaming, a decode-thread compose_overlay never reads a half-freed
    // buffer / stale dims.
    {
        xSemaphoreTake(overlay_mtx_, portMAX_DELAY);
        if (overlay_buf_) { heap_caps_free(overlay_buf_); overlay_buf_ = nullptr; }
        overlay_rect_ = cfg.rect;
        overlay_rotation_ = cfg.rotation;
        overlay_scale_ = scale;
        content_w_ = new_cw;
        content_h_ = new_ch;
        overlay_buf_ = static_cast<uint8_t *>(
            heap_caps_aligned_alloc(64, buf_bytes, MALLOC_CAP_SPIRAM));
        if (overlay_buf_) std::memset(overlay_buf_, 0, buf_bytes);
        xSemaphoreGive(overlay_mtx_);
    }
    overlay_visible_ = false;  // the caller re-asserts its visibility policy

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

    // Isolate the main display from the panel framebuffers: it renders into a
    // full-screen scratch buffer (and main_flush_cb stops presenting) so the
    // mirror owns the panel framebuffers exclusively — otherwise the main
    // display's one-shot render of its (static) screen leaves stale pixels in a
    // buffer the mirror then composites the strip over when no video is arriving.
    // (Idempotent on re-entry: main_scratch_ is allocated once.)
    size_t fb_bytes = (size_t)PANEL_W * PANEL_H * bsp_pixel_format_bytes(format());
    if (!main_scratch_)
        main_scratch_ = static_cast<uint8_t *>(
            heap_caps_aligned_alloc(64, fb_bytes, MALLOC_CAP_SPIRAM));
    if (main_scratch_)
        lv_display_set_buffers(main_disp_, main_scratch_, nullptr, fb_bytes,
                               LV_DISPLAY_RENDER_MODE_DIRECT);

    // Route touch to the overlay display (read_cb maps panel -> content coords).
    // Reset first so an in-progress press isn't carried onto the smaller display.
    lv_indev_set_display(indev_, overlay_disp_);
    lv_indev_reset(indev_, nullptr);
    // Drop any in-progress press/mask so it isn't carried onto the overlay.
    xSemaphoreTake(touch_mtx_, portMAX_DELAY);
    lvgl_pressed_ = false;
    lvgl_suppress_ = false;
    xSemaphoreGive(touch_mtx_);
    mode_ = Mode::Overlay;

    if (cfg.clear_framebuffers) {
        // Clear every configured framebuffer (the mirror triple-buffers, so there
        // can be three) so the pre-stream / letterbox stays black whichever buffer
        // is presented first. Indices past the configured count return NULL.
        for (int i = 0; i < 3; ++i) {
            void *fb = bsp_display_get_frame_buffer(i);
            if (!fb) continue;
            std::memset(fb, 0, fb_bytes);
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
    lv_indev_reset(indev_, nullptr);
    // Restore the main display onto the panel framebuffers; the next screen's
    // re-render (ScreenManager pop -> lv_screen_load) repaints the panel.
    void  *fb0 = bsp_display_get_frame_buffer(0);
    void  *fb1 = bsp_display_get_frame_buffer(1);
    size_t fb_bytes = (size_t)PANEL_W * PANEL_H * bsp_pixel_format_bytes(format());
    lv_display_set_buffers(main_disp_, fb0, fb1, fb_bytes,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    if (overlay_disp_) { lv_display_delete(overlay_disp_); overlay_disp_ = nullptr; }
    if (overlay_buf_) { heap_caps_free(overlay_buf_); overlay_buf_ = nullptr; }
}

void DisplayManager::set_overlay_visible(bool visible) { overlay_visible_ = visible; }

void DisplayManager::compose_overlay(int index) {
    // Hold overlay_mtx_ for the whole read+composite: reconfigure_overlay (LVGL
    // thread) frees and reallocates overlay_buf_ + geometry, so without this a
    // concurrent rotate-switch could free the buffer mid-PPA. A leaf lock (we
    // never take lv_lock here), so it can't deadlock the teardown join.
    xSemaphoreTake(overlay_mtx_, portMAX_DELAY);
    if (!overlay_buf_) {
        xSemaphoreGive(overlay_mtx_);
        return;
    }
    if (!ppa_srm_) {
        ppa_client_config_t cc = {};
        cc.oper_type = PPA_OPERATION_SRM;
        ppa_client_handle_t client = nullptr;
        if (ppa_register_client(&cc, &client) != ESP_OK) {
            xSemaphoreGive(overlay_mtx_);
            return;
        }
        ppa_srm_ = client;
    }
    void *fb = bsp_display_get_frame_buffer(index);
    if (!fb) {
        xSemaphoreGive(overlay_mtx_);
        return;
    }
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
    xSemaphoreGive(overlay_mtx_);
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
