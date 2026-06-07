// Headless test for adb::Sync (run on the desktop against a real Android
// device). Exercises slice 5, the Tab5->Android direction: connect_usb() ->
// open_sync() -> push() a small in-memory buffer to /data/local/tmp -> stat()
// it back and check it exists with the right size -> close() -> on_sync_close.
// The reader + worker tasks run on the pthread-backed FreeRTOS compat, so this
// links the idf_compat freertos sources too.
//
// The phone must already be authorized (run test_client once and tap Allow).
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
//       test/test_sync.cpp src/adb_client.cpp src/adb_error.cpp src/adb_shell.cpp \
//       src/adb_sync.cpp \
//       ../embedded_adb/src/adb_protocol.cpp ../embedded_adb/src/adb_crypto.cpp \
//       ../embedded_adb/src/adb_keystore.cpp ../embedded_adb/src/adb_connection.cpp \
//       ../embedded_adb/src/adb_stream.cpp ../embedded_adb/src/transport_libusb.cpp \
//       *.o \
//       $(pkg-config --cflags --libs mbedtls mbedcrypto mbedx509 libusb-1.0 libcjson) \
//       -lpthread -o /tmp/test_sync && /tmp/test_sync
#include "adb.hpp"

#include <nvs_flash.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

class Listener : public adb::ClientListener, public adb::SyncListener {
public:
    // --- ClientListener ---
    void on_state(adb::Client*, adb::ConnectionState s) override {
        std::printf(">>> %s\n", adb::to_string(s));
        if (s == adb::ConnectionState::Online) online_ = true;
        if (s == adb::ConnectionState::Unauthorized) {
            std::printf("    -> authorize the device first (run test_client)\n");
        }
    }
    bool online() const { return online_; }

    // --- SyncListener ---
    void on_sync_close(adb::Sync*, adb::Error err) override {
        std::printf(">>> sync closed (%s)\n", adb::to_string(err));
        ++closes_;
    }
    int closes() const { return closes_; }

private:
    std::atomic<bool> online_{false};
    std::atomic<int> closes_{0};
};

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

int main() {
    nvs_flash_sim_set_path("/tmp/adb_test_nvs.json");

    auto listener = std::make_shared<Listener>();  // held weakly by Client/Sync
    auto client = adb::Client::connect_usb(listener);

    for (int i = 0; i < 200 && !listener->online(); ++i) {
        if (client->state() == adb::ConnectionState::Closed) break;
        sleep_ms(100);
    }
    if (!listener->online()) {
        std::printf("FAIL: did not reach Online\n");
        return 1;
    }

    auto sync = client->open_sync(listener);
    if (!sync) {
        std::printf("FAIL: open_sync returned nullptr\n");
        return 1;
    }

    // The payload to push (Tab5 -> Android), served from memory.
    const std::string remote = "/data/local/tmp/tab5_sync_test.txt";
    std::string payload = "hello from tab5 sync\n";
    payload.append(5000, 'x');  // span multiple DATA chunks on a 16 KiB link
    payload += "\nEND\n";

    // Push, then read the device's stat back to confirm it landed.
    std::atomic<bool> push_done{false};
    std::atomic<adb::Error> push_err{adb::Error::Ok};
    size_t off = 0;
    sync->push(
        remote, 0644, /*mtime=*/0,
        [&](uint8_t* buf, size_t cap) -> int {
            size_t n = std::min(cap, payload.size() - off);
            std::memcpy(buf, payload.data() + off, n);
            off += n;
            return static_cast<int>(n);  // 0 at EOF
        },
        [&](adb::Error err) {
            push_err = err;
            push_done = true;
        });
    for (int i = 0; i < 100 && !push_done; ++i) sleep_ms(100);

    std::atomic<bool> stat_done{false};
    adb::FileStat st;
    sync->stat(remote, [&](adb::Error err, adb::FileStat s) {
        if (err == adb::Error::Ok) st = s;
        stat_done = true;
    });
    for (int i = 0; i < 100 && !stat_done; ++i) sleep_ms(100);

    bool pushed = push_done && push_err.load() == adb::Error::Ok;
    bool landed = st.exists() && st.is_reg() && st.size == payload.size();
    std::printf("push=%s (err=%s), stat: exists=%d size=%u (want %zu)\n",
                pushed ? "ok" : "FAILED", adb::to_string(push_err.load()),
                st.exists(), st.size, payload.size());

    sync->close();
    sync.reset();   // drop the sync; the weak listener ref expires with it
    client->close();

    bool one_close = listener->closes() == 1;
    bool ok = pushed && landed && one_close;
    std::printf("%s (closes=%d)\n",
                ok ? "PASSED: push lands on the device and sync closes once"
                   : "FAILED",
                listener->closes());
    return ok ? 0 : 1;
}
