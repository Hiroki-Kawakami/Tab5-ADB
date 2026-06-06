#include "adb_app.hpp"
#include "platform_port.hpp"
#include "lvgl.hpp"
#include "screen_manager.hpp"
#include "home_screen.hpp"

// Two BSP frame buffers drive LVGL in DIRECT mode: LVGL renders straight into
// the panel buffers and flush_cb hands the finished buffer to the BSP.
static void lvgl_setup() {
    void *fb0 = pf_port::display_get_frame_buffer(0);
    void *fb1 = pf_port::display_get_frame_buffer(1);
    size_t bpp = pf_port::bytes_per_pixel(pf_port::display_pixel_format());

    auto disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_color_format(
        disp,
        pf_port::display_pixel_format() == pf_port::PixelFormat::RGB888
            ? LV_COLOR_FORMAT_RGB888
            : LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, fb0, fb1, PANEL_W * PANEL_H * bpp,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, [](lv_display_t *disp, const lv_area_t *area, uint8_t *px_map){
        // Map the rendered buffer back to its BSP frame-buffer index.
        int fb_index = (px_map == pf_port::display_get_frame_buffer(1)) ? 1 : 0;
        pf_port::display_flush(fb_index);
        lv_display_flush_ready(disp);
    });

    auto indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, [](lv_indev_t *indev, lv_indev_data_t *data){
        auto touch = pf_port::touch_get_point();
        if (touch.has_value()) {
            data->state = LV_INDEV_STATE_PRESSED;
            data->point.x = std::get<0>(touch.value());
            data->point.y = std::get<1>(touch.value());
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    });
}

void adb_app() {
    pf_port::init(2, pf_port::PixelFormat::RGB565);
    lvgl_setup();
    lv_async_call([](){
        screen_manager.push(std::make_unique<HomeScreen>());
    });
    pf_port::display_set_brightness(80);
}
