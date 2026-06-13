// Headless ADB-over-TCP connect test (run on the desktop against a real Android
// device already listening on TCP — `adb tcpip 5555` while plugged in, or wireless
// debugging). Opens the TCP transport, loads/creates the RSA identity, runs CNXN +
// AUTH, then a `shell:` round-trip.
//
// The target host:port comes from the TAB5ADB_TCP_TARGET env var so no IP/port is
// committed (it varies per device/session). On first run with this test's fresh
// key the phone shows "Allow wireless debugging?" — tap Allow.
//
//   TAB5ADB_TCP_TARGET=192.168.x.y:zzzzz \
//     TEST=test_connect_tcp nix develop -c components/embedded_adb/test/run.sh
#include "embedded_adb.hpp"

#include <nvs_flash.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

int main() {
    const char* target = std::getenv("TAB5ADB_TCP_TARGET");
    if (!target || !*target) {
        std::printf("SKIP: set TAB5ADB_TCP_TARGET=host:port to run this test\n");
        return 0;
    }
    std::string t = target;
    size_t colon = t.rfind(':');
    if (colon == std::string::npos) {
        std::printf("FAIL: TAB5ADB_TCP_TARGET must be host:port\n");
        return 1;
    }
    std::string host = t.substr(0, colon);
    uint16_t port = static_cast<uint16_t>(std::atoi(t.c_str() + colon + 1));
    std::printf("target: %s:%u\n", host.c_str(), port);

    // Keep the test's NVS out of the way (don't touch any real nvs_data.json).
    nvs_flash_sim_set_path("/tmp/adb_test_nvs.json");

    auto key = adb::load_or_create_key();
    if (!key) {
        std::printf("FAIL: could not load/create RSA key\n");
        return 1;
    }
    std::printf("RSA identity ready\n");

    auto transport = adb::open_tcp_transport(host, port);
    if (!transport) {
        std::printf("FAIL: could not connect to %s:%u (is the device listening?)\n",
                    host.c_str(), port);
        return 1;
    }

    adb::AdbConnection conn(std::move(transport), std::move(*key));
    conn.set_state_callback([&conn](adb::ConnectionState s) {
        std::printf(">>> %s\n", adb::to_string(s));
        if (s == adb::ConnectionState::Unauthorized)
            std::printf("    -> tap \"Allow wireless debugging?\" on the phone\n");
        if (s == adb::ConnectionState::Online) {
            std::printf("    BANNER: %s\n", conn.banner().c_str());
            std::printf("    maxdata: %u\n", conn.max_payload());
        }
    });

    // Run the read loop on a worker thread so we can issue a shell command.
    std::thread reader([&conn] { conn.run_blocking(); });

    // Wait briefly for Online, then run a command.
    bool ok = false;
    for (int i = 0; i < 200 && conn.state() != adb::ConnectionState::Online; i++) {
        if (conn.state() == adb::ConnectionState::Closed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (conn.state() == adb::ConnectionState::Online) {
        std::string out;
        if (conn.run_service("shell:echo tab5adb-tcp-ok", out)) {
            std::printf("    shell echo -> %s", out.c_str());
            ok = out.find("tab5adb-tcp-ok") != std::string::npos;
        }
    }

    conn.stop();
    reader.join();
    std::printf("%s\n", ok ? "PASSED: Online + shell round-trip"
                           : "FAILED: did not complete");
    return ok ? 0 : 1;
}
