#include "lvgl.h"
#include "adb_app.hpp"

#include <SDL2/SDL.h>

#include <unistd.h>

// Simulator entry. SDL/LVGL must own the main thread on macOS, so the main
// thread runs the LVGL/SDL loop. FreeRTOS here is the host pthread-backed compat
// layer (simulator/idf_compat) — there is no scheduler to start and no main-
// thread restriction, so adb_app() may create tasks directly, just like on the
// device. The device counterpart is esp32p4/main/main.cpp.
extern "C" int main(void) {
    lv_init();
    // LVGL runtime time source on the host (the device gets this from
    // esp_lvgl_port). SDL is initialised by bsp_init via the SDL backend.
    lv_tick_set_cb(SDL_GetTicks);
    lv_delay_set_cb(SDL_Delay);

    // Shared app entry: bsp_init (SDL display/touch) + LVGL display setup, and
    // any module init (which may freely spawn FreeRTOS tasks). On device the
    // equivalent is app_main().
    adb_app();

    while (1) {
        uint32_t sleep_time_ms = lv_timer_handler();
        if (sleep_time_ms == LV_NO_TIMER_READY) {
            sleep_time_ms = LV_DEF_REFR_PERIOD;
        }
        usleep(sleep_time_ms * 1000);
    }
    return 0;
}
