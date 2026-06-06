#include <cstdio>
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "bsp_tab5.h"
#include "platform_port.hpp"
#include "adb_app.hpp"

static const char *TAG = "main";

namespace pf_port {

static PixelFormat s_pixel_format = PixelFormat::RGB565;

PixelFormat display_pixel_format() {
    return s_pixel_format;
}

void init(int fb_num, PixelFormat pixel_format) {
    s_pixel_format = pixel_format;
    bsp_tab5_config_t bsp_config = {};
    bsp_config.display.fb_num = fb_num;
    bsp_config.display.pixel_format = (pixel_format == PixelFormat::RGB888)
        ? BSP_PIXEL_FORMAT_RGB888
        : BSP_PIXEL_FORMAT_RGB565;
    bsp_config.usb.usb5v_en = true;
    ESP_ERROR_CHECK(bsp_tab5_init(&bsp_config));

    lvgl_port_cfg_t config = {
        .task_priority = 4,
        .task_stack = 7168,
        .task_affinity = 1,
        .task_max_sleep_ms = 500,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms = 5,
    };
    esp_err_t err = lvgl_port_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start LVGL: %s", esp_err_to_name(err));
        assert(0);
    }
}

void display_set_brightness(int value) {
    bsp_tab5_display_set_brightness(value);
}

void *display_get_frame_buffer(int fb_index) {
    return bsp_tab5_display_get_frame_buffer(fb_index);
}

void display_flush(int fb_index) {
    bsp_tab5_display_flush(fb_index);
}

std::optional<std::tuple<int, int>> touch_get_point() {
    esp_lcd_touch_point_data_t touch;
    int touch_num = bsp_tab5_touch_read(&touch, 1);
    if (touch_num > 0) {
        return std::make_tuple(touch.x, touch.y);
    }
    return std::nullopt;
}

}

extern "C" void app_main() {
    adb_app();
}
