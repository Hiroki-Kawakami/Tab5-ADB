# Simulator-only ESP-IDF compatibility layer. See README.md for the full picture
# (what lives here, the include-path rules, and how to upgrade the vendored
# FreeRTOS-Kernel). Consumed via the idf_component_register() shim in
# simulator/CMakeLists.txt, which folds these straight into the `simulator` exe.
#
# Layout: include/ = shim headers, src/ = shim sources, freertos_kernel/ =
# vendored upstream FreeRTOS (GCC/Posix port subset, with the macOS fixes applied
# inline — grep "macOS/arm64" in port.c).
idf_component_register(
    SRCS
        # hand-written host shims
        "src/esp_err.c"
        "src/esp_timer.c"
        "src/esp_heap_caps.c"
        "src/nvs.c"
        # vendored FreeRTOS-Kernel (GCC/Posix port)
        "freertos_kernel/tasks.c"
        "freertos_kernel/queue.c"
        "freertos_kernel/list.c"
        "freertos_kernel/timers.c"
        "freertos_kernel/event_groups.c"
        "freertos_kernel/stream_buffer.c"
        "freertos_kernel/portable/ThirdParty/GCC/Posix/port.c"
        "freertos_kernel/portable/ThirdParty/GCC/Posix/utils/wait_for_event.c"
        "freertos_kernel/portable/MemMang/heap_3.c"
    INCLUDE_DIRS
        # shim headers (bare-name: "esp_err.h"; the "freertos/" bridge subdir is
        # reachable as "freertos/FreeRTOS.h" but is NOT itself on the path, so
        # the bridge's #include <FreeRTOS.h> reaches the kernel, not itself)
        "include"
        # vendored FreeRTOS: bare-name headers, the Posix port, and the config
        "freertos_kernel/include"
        "freertos_kernel/portable/ThirdParty/GCC/Posix"
        "freertos_kernel")
