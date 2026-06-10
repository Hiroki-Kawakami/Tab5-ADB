# Shared application logic (screens, app entry point adb_app()).
# Discovered as a single component via EXTRA_COMPONENT_DIRS on device and via
# the idf_component_register() shim in simulator/CMakeLists.txt on the host.
#
# SRCS is collected recursively so new .c/.cpp files under app/ are picked up
# automatically. CONFIGURE_DEPENDS is unusable here because ESP-IDF evaluates
# this file in cmake script mode (cmake -P) for requirement extraction, where it
# is invalid — so re-run cmake (or delete build/) after adding/removing files.
file(GLOB_RECURSE APP_SRCS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS "."
    REQUIRES m5stack-bsp "lvgl++" screen_manager lvgl adb agent_link
             jpeg_decode_enhanced esp_driver_jpeg esp_driver_ppa
             esp_timer zlib)
