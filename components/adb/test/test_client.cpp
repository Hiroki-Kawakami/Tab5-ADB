// Headless test for adb::Client (run on the desktop against a real Android
// device). Exercises the first slice: connect_usb() -> state()/banner() ->
// close(). The reader task runs on the pthread-backed FreeRTOS compat, so this
// links the idf_compat freertos sources too.
//
// First run with a fresh key: the phone shows "Allow USB debugging?" — tap Allow.
//
// Build & run (inside `nix develop`, phone connected, `adb kill-server` first so
// nothing else holds the interface). From components/adb:
//   cc=../../simulator/idf_compat
//   gcc -c $cc/src/nvs.c $cc/src/esp_err.c \
//       $cc/src/freertos_task.c $cc/src/freertos_queue.c \
//       $cc/src/freertos_port.c $cc/src/freertos_timers.c \
//       $cc/src/freertos_event_groups.c $cc/src/esp_timer.c \
//       -I$cc/include
//   g++ -std=c++17 -Iinc -I../embedded_adb/inc -I$cc/include \
//       test/test_client.cpp src/adb_client.cpp src/adb_error.cpp \
//       ../embedded_adb/src/adb_protocol.cpp ../embedded_adb/src/adb_crypto.cpp \
//       ../embedded_adb/src/adb_keystore.cpp ../embedded_adb/src/adb_connection.cpp \
//       ../embedded_adb/src/adb_stream.cpp ../embedded_adb/src/transport_libusb.cpp \
//       *.o \
//       $(pkg-config --cflags --libs mbedtls mbedcrypto mbedx509 libusb-1.0 libcjson) \
//       -lpthread -o /tmp/test_client && /tmp/test_client
#include "adb.hpp"

#include <nvs_flash.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace {

// Collects state transitions; the Client* first arg is exercised but a single
// device is enough here.
class Listener : public adb::ClientListener {
public:
    void on_state(adb::Client* c, adb::ConnectionState s) override {
        std::printf(">>> %s\n", adb::to_string(s));
        if (s == adb::ConnectionState::Unauthorized) {
            std::printf("    -> tap \"Allow USB debugging?\" on the phone\n");
        }
        if (s == adb::ConnectionState::Online) {
            std::printf("    BANNER: %s\n", c->banner().c_str());
            online_ = true;
        }
    }
    bool online() const { return online_; }

private:
    bool online_ = false;
};

}  // namespace

int main() {
    nvs_flash_sim_set_path("/tmp/adb_test_nvs.json");  // don't touch a real store

    Listener listener;
    auto client = adb::Client::connect_usb(&listener);

    // Poll for Online (the reader task drives state asynchronously). ~20s budget
    // covers a first-run authorization tap.
    for (int i = 0; i < 200 && !listener.online(); ++i) {
        if (client->state() == adb::ConnectionState::Closed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    bool ok = listener.online();
    std::printf("state() = %s, banner empty = %s\n",
                adb::to_string(client->state()), client->banner().empty() ? "yes" : "no");

    std::printf("closing...\n");
    client->close();  // must return promptly (reader task joined)
    std::printf("closed, state() = %s\n", adb::to_string(client->state()));

    std::printf("%s\n", ok ? "PASSED: reached Online and closed cleanly"
                           : "ended without Online");
    return ok ? 0 : 1;
}
