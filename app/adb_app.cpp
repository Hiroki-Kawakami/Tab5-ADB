#include "adb_app.hpp"
#include "bsp.h"
#include "lvgl.hpp"
#include "screen_manager.hpp"
#include "home_screen.hpp"

// Two BSP frame buffers drive LVGL in DIRECT mode: LVGL renders straight into
// the panel buffers and flush_cb hands the finished buffer to the BSP. The BSP
// (device drivers or the simulator's SDL backend) is the single hardware seam,
// so this setup is identical on both targets.
static void lvgl_setup() {
    bsp_pixel_format_t format = bsp_display_get_pixel_format();
    void *fb0 = bsp_display_get_frame_buffer(0);
    void *fb1 = bsp_display_get_frame_buffer(1);
    size_t bpp = bsp_pixel_format_bytes(format);

    auto disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_color_format(
        disp,
        format == BSP_PIXEL_FORMAT_RGB888 ? LV_COLOR_FORMAT_RGB888
                                          : LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, fb0, fb1, PANEL_W * PANEL_H * bpp,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, [](lv_display_t *disp, const lv_area_t *area, uint8_t *px_map){
        // Map the rendered buffer back to its BSP frame-buffer index.
        int fb_index = (px_map == bsp_display_get_frame_buffer(1)) ? 1 : 0;
        bsp_display_flush(fb_index);
        lv_display_flush_ready(disp);
    });

    auto indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, [](lv_indev_t *indev, lv_indev_data_t *data){
        bsp_touch_point_t point;
        if (bsp_touch_read(&point, 1) > 0) {
            data->state = LV_INDEV_STATE_PRESSED;
            data->point.x = point.x;
            data->point.y = point.y;
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    });
}

void adb_app() {
    bsp_config_t config = {};
    config.display.fb_num = 2;
    config.display.pixel_format = BSP_PIXEL_FORMAT_RGB565;
    config.usb.usb5v_en = true;
    ESP_ERROR_CHECK(bsp_init(&config));

    lvgl_setup();
    lv_async_call([](){
        screen_manager.push(std::make_unique<HomeScreen>());
    });
    bsp_display_set_brightness(80);
}
