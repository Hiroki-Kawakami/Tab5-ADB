# embedded_adb — shared ADB host-side client library (see inc/embedded_adb.hpp).
#
# Like m5stack-bsp, this is a target-divergent shared component: everything is
# portable C++ EXCEPT the USB transport, which has an esp-idf usb_host backend on
# device and a libusb backend in the simulator. The transport split is selected
# here by ESP_PLATFORM (defined only under ESP-IDF). Until the transport lands
# (P3) both branches register the same portable sources.
set(ADB_COMMON_SRCS
    "src/adb_protocol.cpp"
    "src/adb_crypto.cpp"      # mbedTLS RSA auth (both targets)
    "src/adb_keystore.cpp"    # NVS key persistence (both targets)
    "src/adb_connection.cpp"  # CNXN handshake + AUTH state machine
    "src/adb_stream.cpp")     # A_OPEN/OKAY/WRTE/CLSE stream multiplexing

if(ESP_PLATFORM)
    # Device: esp-idf usb_host transport (stub until P6).
    idf_component_register(
        SRCS ${ADB_COMMON_SRCS}
            "src/transport_usbhost.cpp"
        INCLUDE_DIRS "inc"
        PRIV_INCLUDE_DIRS "src"
        PRIV_REQUIRES mbedtls nvs_flash usb)
else()
    # Simulator: libusb transport. libusb + mbedTLS are linked to the `simulator`
    # executable in simulator/CMakeLists.txt, so their headers are on the global
    # include path; NVS is the idf_compat JSON-backed shim.
    idf_component_register(
        SRCS ${ADB_COMMON_SRCS}
            "src/transport_libusb.cpp"
        INCLUDE_DIRS "inc"
        PRIV_INCLUDE_DIRS "src")
endif()
