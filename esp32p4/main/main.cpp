// Device entry point. Hardware now lives behind the shared BSP (bsp_init is
// called from adb_app), so main only owns the device-side LVGL runtime
// (esp_lvgl_port) before handing off to the shared app. The simulator's
// counterpart (LVGL + SDL/FreeRTOS loop) is simulator/platform/main.cpp.

#include <cassert>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "adb_app.hpp"
#include "adb_smoketest.hpp"

static const char *TAG = "main";

extern "C" void app_main() {
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
    adb_app();

    // P6 bring-up: exercise the usb_host ADB transport over the serial console.
    // (BSP powered the USB host port in adb_app's bsp_init.)
    adb_smoketest_start();
}
