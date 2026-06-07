# adb — app-facing, object-oriented ADB API over embedded_adb (see README.md).
#
# Unlike embedded_adb (the thread-agnostic protocol engine), this component owns
# the connection lifecycle and runs the packet read loop on a FreeRTOS task
# (real kernel on device, pthread-backed compat in the simulator). No
# device/simulator source split — everything here is portable C++ on top of
# embedded_adb + the FreeRTOS API.
idf_component_register(
    SRCS "src/adb_client.cpp"
         "src/adb_error.cpp"
         "src/adb_shell.cpp"
    INCLUDE_DIRS "inc"
    REQUIRES embedded_adb
    PRIV_REQUIRES freertos)
