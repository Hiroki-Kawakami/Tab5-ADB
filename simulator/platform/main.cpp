#include "lvgl.h"
#include "FreeRTOS.h"
#include "task.h"
#include "adb_app.hpp"

#include <SDL2/SDL.h>

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void *freertos_scheduler_thread(void *) {
    // vTaskStartScheduler() blocks for the lifetime of the program on the
    // POSIX port. Runs off the main thread so the LVGL/SDL loop below can own
    // the main thread (required by SDL_PollEvent on macOS).
    vTaskStartScheduler();
    fprintf(stderr, "vTaskStartScheduler returned unexpectedly\n");
    return nullptr;
}

extern "C" int main(void) {
    // The FreeRTOS POSIX port uses SIGALRM as the tick signal (delivered via
    // pthread_kill to the currently-running task) and SIGUSR1 (SIG_RESUME) to
    // wake suspended task threads. Any thread that isn't a FreeRTOS task must
    // mask both, or the kernel may deliver them to the LVGL/main thread.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGALRM);
    sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);

    lv_init();
    // LVGL runtime time source on the host (the device gets this from
    // esp_lvgl_port). SDL is initialised later by bsp_init via the SDL backend;
    // these callbacks are only invoked from the lv_timer_handler loop below,
    // which runs after app_main() has brought SDL up.
    lv_tick_set_cb(SDL_GetTicks);
    lv_delay_set_cb(SDL_Delay);
    // The simulator's app entry: bsp_init (SDL display/touch) + LVGL display
    // setup live in adb_app(). On device the equivalent is app_main().
    adb_app();

    pthread_t freertos_tid;
    if (pthread_create(&freertos_tid, nullptr, freertos_scheduler_thread, nullptr) != 0) {
        fprintf(stderr, "pthread_create(freertos) failed\n");
        return 1;
    }
    pthread_detach(freertos_tid);

    while (1) {
        uint32_t sleep_time_ms = lv_timer_handler();
        if (sleep_time_ms == LV_NO_TIMER_READY) {
            sleep_time_ms = LV_DEF_REFR_PERIOD;
        }
        usleep(sleep_time_ms * 1000);
    }
    return 0;
}
