// Headless shell test (run on the desktop against a real Android device).
// Connects (must already be authorized — run test_connect first), then runs a
// few `shell:` commands and prints their output. The read loop runs on a
// std::thread; the main thread issues run_service() calls.
//
// Build & run with the test runner (builds everything, runs `adb kill-server`,
// launches the test):
//   TEST=test_shell nix develop -c components/embedded_adb/test/run.sh
// (See test/run.sh for the underlying g++/gcc command if you need it by hand.)
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
