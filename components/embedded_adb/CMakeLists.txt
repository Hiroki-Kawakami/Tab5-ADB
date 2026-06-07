# embedded_adb — shared ADB host-side client library (see inc/embedded_adb.hpp).
#
# Like m5stack-bsp, this is a target-divergent shared component: everything is
# portable C++ EXCEPT the USB transport, which has an esp-idf usb_host backend on
# device and a libusb backend in the simulator. The transport split is selected
# here by ESP_PLATFORM (defined only under ESP-IDF). Until the transport lands
# (P3) both branches register the same portable sources.
set(ADB_COMMON_SRCS
    "src/adb_protocol.cpp")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${ADB_COMMON_SRCS}
        INCLUDE_DIRS "inc"
        PRIV_INCLUDE_DIRS "src")
else()
    # Simulator: libusb + mbedTLS are linked to the `simulator` executable in
    # simulator/CMakeLists.txt, so their headers are on the global include path.
    idf_component_register(
        SRCS ${ADB_COMMON_SRCS}
        INCLUDE_DIRS "inc"
        PRIV_INCLUDE_DIRS "src")
endif()
