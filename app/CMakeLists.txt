# Shared application logic (screens, app entry point adb_app()).
# Discovered as a single component via EXTRA_COMPONENT_DIRS on device and via
# the idf_component_register() shim in simulator/CMakeLists.txt on the host.
idf_component_register(
    SRCS "adb_app.cpp" "home_screen.cpp" "adb_device_screen.cpp"
    INCLUDE_DIRS "."
    REQUIRES m5stack-bsp "lvgl++" screen_manager lvgl adb)
