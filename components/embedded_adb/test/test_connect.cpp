// Headless connect test (run on the desktop against a real Android device).
// Opens the libusb transport, loads/creates the RSA identity, and runs the CNXN
// + AUTH handshake. On first run with a new key the phone shows "Allow USB
// debugging?" — tap Allow; the device then sends CNXN and we go Online.
//
// Build & run (inside `nix develop`, with a phone connected and `adb kill-server`
// done so nothing else holds the interface):
//   g++ -std=c++17 -Iinc -I../../simulator/idf_compat/include \
//       test/test_connect.cpp src/adb_protocol.cpp src/adb_crypto.cpp \
//       src/adb_keystore.cpp src/transport_libusb.cpp src/adb_connection.cpp \
//       ../../simulator/idf_compat/src/nvs.c ../../simulator/idf_compat/src/esp_err.c \
//       $(pkg-config --cflags --libs mbedtls mbedcrypto mbedx509 libusb-1.0 libcjson) \
//       -lpthread -o /tmp/test_connect && /tmp/test_connect
#include "embedded_adb.hpp"

#include <nvs_flash.h>

#include <cstdio>

int main() {
    // Keep the test's NVS out of the way (don't touch any real nvs_data.json).
    nvs_flash_sim_set_path("/tmp/adb_test_nvs.json");

    auto key = adb::load_or_create_key();
    if (!key) {
        std::printf("FAIL: could not load/create RSA key\n");
        return 1;
    }
    std::printf("RSA identity ready\n");

    auto transport = adb::open_usb_transport();
    if (!transport) {
        std::printf("FAIL: no ADB device (connect a phone, enable USB debugging, "
                    "and run `adb kill-server`)\n");
        return 1;
    }

    adb::AdbConnection conn(std::move(transport), std::move(*key));
    conn.set_state_callback([&conn](adb::ConnectionState s) {
        std::printf(">>> %s\n", adb::to_string(s));
        if (s == adb::ConnectionState::Unauthorized) {
            std::printf("    -> tap \"Allow USB debugging?\" on the phone\n");
        }
        if (s == adb::ConnectionState::Online) {
            std::printf("    BANNER: %s\n", conn.banner().c_str());
            std::printf("    maxdata: %u\n", conn.max_payload());
            conn.stop();
        }
    });

    conn.run_blocking();

    bool ok = !conn.banner().empty();
    std::printf("%s\n", ok ? "PASSED: reached Online" : "ended without Online");
    return ok ? 0 : 1;
}
