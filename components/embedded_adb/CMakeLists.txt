# embedded_adb — shared ADB host-side client library (see inc/embedded_adb.hpp).
#
# This is a target-divergent shared component: everything is
# portable C++ EXCEPT the USB transport, which has an esp-idf usb_host backend on
# device and a libusb backend in the simulator. The transport split is selected
# here by ESP_PLATFORM (defined only under ESP-IDF). The TCP transport is the
# exception: BSD sockets / lwip are a standard contract, so transport_tcp.cpp is
# the SAME on both targets and lives in the common sources, not the split.
set(ADB_COMMON_SRCS
    "src/adb_protocol.cpp"
    "src/adb_crypto.cpp"             # mbedTLS RSA auth (both targets)
    "src/adb_keystore.cpp"           # NVS key persistence (both targets)
    "src/adb_spake2.cpp"             # ADB-compatible SPAKE2 over Edwards25519
    "src/adb_pairing_crypto.cpp"     # pairing HKDF + AES-128-GCM
    "src/adb_pairing_protocol.cpp"   # pairing header codec
    "src/adb_pairing.cpp"            # six-digit wireless-debugging pairing
    "src/adb_tcp_socket.cpp"         # TCP connect + socket lifetime
    "src/adb_tls_stream.cpp"         # shared TLS 1.3 stream + exporter
    "src/adb_connection.cpp"         # CNXN handshake + AUTH state machine
    "src/adb_stream.cpp"             # A_OPEN/OKAY/WRTE/CLSE stream multiplexing
    "src/transport_tcp.cpp")         # ADB-over-TCP (sockets — same on both targets)

if(ESP_PLATFORM)
    # Device: esp-idf usb_host transport (stub until P6).
    idf_component_register(
        SRCS ${ADB_COMMON_SRCS}
            "src/transport_usbhost.cpp"
        INCLUDE_DIRS "inc"
        PRIV_INCLUDE_DIRS "src"
        PRIV_REQUIRES mbedtls nvs_flash usb lwip)  # lwip = BSD sockets for transport_tcp
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
