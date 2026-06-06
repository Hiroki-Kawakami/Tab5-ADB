# Simulator-only ESP-IDF API compatibility layer.
#
# Host implementations of the ESP-IDF symbols / headers that shared code (app/,
# components/) uses but that only really exist on-device: esp_err, esp_log, the
# `freertos/*.h` include-path shims, and whatever else gets added as this layer
# grows. On device these are the real ESP-IDF components, so this directory is
# NOT built there (it lives under simulator/, outside EXTRA_COMPONENT_DIRS).
#
# Self-describing like every other component: it declares its sources / includes
# with idf_component_register(), consumed via the shim in simulator/CMakeLists.txt
# that folds them into the `simulator` executable.
#
# INCLUDE_DIRS is just "." on purpose — `idf_compat/freertos/` must stay OFF the
# include path so the freertos shims' `#include <task.h>` reaches the real
# kernel, not themselves.
idf_component_register(
    SRCS "esp_err.c"
    INCLUDE_DIRS ".")
