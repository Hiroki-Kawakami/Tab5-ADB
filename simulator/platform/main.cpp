#include "adb_app.hpp"
#include "esp_heap_caps.h"
#include "lvgl.hpp"
#include "sim_harness.h"
#include "wifi_sim.hpp"

#include <cstdlib>

extern "C" int main(void) {
    lvgl_port_cfg_t config = {
        .task_priority = 4,
        .task_stack = 7168,
        .task_affinity = 1,
        .task_max_sleep_ms = 500,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms = 5,
    };
    if (lvgl_port_init(&config) != ESP_OK) return 1;

    adb_app();
    wifi::sim::register_harness_commands();
    sim_harness_start(std::getenv("SIMULATOR_SCRIPT"));
    lvgl_sim_loop(sim_harness_frame);
    return sim_harness_exit_code();
}
