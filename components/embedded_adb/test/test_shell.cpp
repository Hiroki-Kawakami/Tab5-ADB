// Headless shell test (run on the desktop against a real Android device).
// Connects (must already be authorized — run test_connect first), then runs a
// few `shell:` commands and prints their output. The read loop runs on a
// std::thread; the main thread issues run_service() calls.
//
// Build & run (inside `nix develop`, phone connected, `adb kill-server` done):
//   gcc -std=c11 -I../../simulator/idf_compat/include $(pkg-config --cflags libcjson) \
//       -c ../../simulator/idf_compat/src/nvs.c -o /tmp/nvs.o
//   gcc -std=c11 -I../../simulator/idf_compat/include \
//       -c ../../simulator/idf_compat/src/esp_err.c -o /tmp/esp_err.o
//   g++ -std=c++17 -Iinc -I../../simulator/idf_compat/include \
//       test/test_shell.cpp src/adb_protocol.cpp src/adb_crypto.cpp \
//       src/adb_keystore.cpp src/transport_libusb.cpp src/adb_connection.cpp \
//       src/adb_stream.cpp /tmp/nvs.o /tmp/esp_err.o \
//       $(pkg-config --cflags --libs mbedtls mbedcrypto mbedx509 libusb-1.0 libcjson) \
//       -lpthread -o /tmp/test_shell && /tmp/test_shell
#include "embedded_adb.hpp"

#include <nvs_flash.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    nvs_flash_sim_set_path("/tmp/adb_test_nvs.json");

    auto key = adb::load_or_create_key();
    auto transport = adb::open_usb_transport();
    if (!key || !transport) {
        std::printf("FAIL: no key or no device\n");
        return 1;
    }

    adb::AdbConnection conn(std::move(transport), std::move(*key));
    std::atomic<bool> online{false};
    conn.set_state_callback([&](adb::ConnectionState s) {
        std::printf(">>> %s\n", adb::to_string(s));
        if (s == adb::ConnectionState::Online) online = true;
    });

    std::thread reader([&conn] { conn.run_blocking(); });

    for (int i = 0; i < 200 && !online; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!online) {
        std::printf("FAIL: did not reach Online (authorize the device first)\n");
        conn.stop();
        reader.join();
        return 1;
    }

    const char* services[] = {
        "shell:getprop ro.product.model",
        "shell:echo hello from tab5",
        "shell:id",
    };
    int failures = 0;
    for (const char* svc : services) {
        std::string out;
        bool ok = conn.run_service(svc, out, 8000);
        std::printf("\n--- %s ---\n%s[ok=%d, %zu bytes]\n", svc, out.c_str(), ok,
                    out.size());
        if (!ok || out.empty()) ++failures;
    }

    conn.stop();
    reader.join();
    std::printf("\n%s\n", failures ? "FAILED" : "PASSED: shell streams work");
    return failures ? 1 : 0;
}
