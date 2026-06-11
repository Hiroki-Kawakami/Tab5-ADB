# m5stack-bsp — shared board-support component.
#
# The model-agnostic glue (src/) is built for both targets; the per-model
# bring-up under boards/<model>/ has a device variant (<model>.c, using the
# real chip drivers in devices/) and a simulator variant (<model>_sim.cpp,
# using the reusable SDL backend in simulator/). The build selects the variant
# by target: ESP_PLATFORM is defined only under ESP-IDF (device); the
# simulator consumes this file through the idf_component_register shim in
# simulator/CMakeLists.txt, where ESP_PLATFORM is unset.
# Model-agnostic display/touch dispatch — built for both targets. (audio_eq.c
# is device-only for now: it pulls in FreeRTOS and there is no host audio path
# yet — it moves into BSP_COMMON_SRCS once the simulator grows an audio board.)
set(BSP_COMMON_SRCS
    "src/bsp_display.c"
    "src/bsp_touch.c")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS
            ${BSP_COMMON_SRCS}
            "src/audio_eq.c"
            "boards/tab5/tab5.c"
            "boards/tab5/tab5_sd.c"
            "devices/pi4io/pi4io.c"
            "devices/ili9881c/ili9881c.c"
            "devices/gt911/gt911.c"
            "devices/st7123/st7123_lcd.c"
            "devices/st7123/st7123_touch.c"
            "devices/es8388/es8388.c"
        INCLUDE_DIRS "inc"
        PRIV_INCLUDE_DIRS "inc_private" "devices"
        PRIV_REQUIRES driver nvs_flash bt fatfs sdmmc esp_driver_sdmmc)

    # Enable Link Time Optimization
    target_compile_options(${COMPONENT_LIB} PRIVATE -flto)
    target_link_libraries(${COMPONENT_LIB} PRIVATE -flto)
else()
    # Simulator: SDL backend + the tab5 host bring-up. SDL2 is already linked to
    # the `simulator` executable (see simulator/CMakeLists.txt), so its headers
    # are on the global include path here.
    idf_component_register(
        SRCS
            ${BSP_COMMON_SRCS}
            "boards/tab5/tab5_sim.c"
            "simulator/sdl_backend.c"
            "simulator/sd_redirect.c"
        INCLUDE_DIRS "inc"
        PRIV_INCLUDE_DIRS "inc_private" "simulator")
endif()
