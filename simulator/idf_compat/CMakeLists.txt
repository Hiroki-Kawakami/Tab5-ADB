# Simulator-only ESP-IDF compatibility layer. See README.md for the full picture
# (what lives here, the include-path rules, and the FreeRTOS API compat design).
# Consumed via the idf_component_register() shim in simulator/CMakeLists.txt,
# which folds these straight into the `simulator` exe.
#
# Layout: include/ = shim headers (esp_* and a host FreeRTOS API), src/ = shim
# sources. The FreeRTOS API is reimplemented on native pthreads (no vendored
# kernel, no scheduler) — same philosophy as the esp_* shims.
idf_component_register(
    SRCS
        # hand-written host shims (ESP-IDF APIs)
        "src/esp_err.c"
        "src/esp_timer.c"
        "src/esp_heap_caps.c"
        "src/nvs.c"
        "src/jpeg_decode.c"   # IDF JPEG decode engine API, backed by libjpeg
        "src/ppa.c"           # IDF PPA (scale/rotate/mirror/blend/fill), CPU impl
        # host FreeRTOS API on pthreads
        "src/freertos_port.c"
        "src/freertos_task.c"
        "src/freertos_queue.c"
        "src/freertos_event_groups.c"
        "src/freertos_timers.c"
    INCLUDE_DIRS
        # shim headers (bare-name: "esp_err.h"; the "freertos/" subdir is
        # reachable as "freertos/FreeRTOS.h" because "include" is on the path)
        "include")

# pthreads (used by the host FreeRTOS API) are linked to the `simulator`
# executable in ../CMakeLists.txt via Threads::Threads — nothing to do here (the
# idf_component_register shim folds sources/includes only; no COMPONENT_LIB).
