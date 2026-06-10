#include "lvgl.h"
#include "adb_app.hpp"
#include "sdl_backend.h"
#include "sim_harness.h"

#include <SDL2/SDL.h>

#include <cstdlib>
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

    // Automated UI verification: if SIMULATOR_SCRIPT names a script, drive the
    // UI headlessly (synthetic touch + framebuffer capture) instead of the
    // interactive SDL loop. The SDL backend honours SIMULATOR_HEADLESS so no
    // host window/display is involved. See sim_harness.h.
    if (const char *script = getenv("SIMULATOR_SCRIPT")) {
        return sim_harness_run(script);
    }

    while (1) {
        // Sample the mouse + drain SDL events on the main thread; the background
        // touch task reads only the snapshot this maintains (SDL is main-thread-only).
        sdl_backend_pump_input();
        uint32_t sleep_time_ms = lv_timer_handler();
        // Present on the main thread: display flushes (incl. from the mirror decode
        // task) only mark the frame dirty, since SDL/Cocoa is main-thread-only.
        sdl_backend_present();
        if (sleep_time_ms == LV_NO_TIMER_READY) {
            sleep_time_ms = LV_DEF_REFR_PERIOD;
        }
        usleep(sleep_time_ms * 1000);
    }
    return 0;
}
