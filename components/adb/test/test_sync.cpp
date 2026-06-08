// Headless test for adb::Sync (run on the desktop against a real Android
// device). Exercises slice 5, the Tab5->Android direction: connect_usb() ->
// open_sync() -> push() a small in-memory buffer to /data/local/tmp -> stat()
// it back and check it exists with the right size -> close() -> on_sync_close.
// The reader + worker tasks run on the pthread-backed FreeRTOS compat, so this
// links the idf_compat freertos sources too.
//
// The phone must already be authorized (run test_client once and tap Allow).
//
// Build & run with the test runner (builds everything, runs `adb kill-server`,
// launches the test):
//   TEST=test_sync nix develop -c components/adb/test/run.sh
// (See test/run.sh for the underlying g++/gcc command if you need it by hand.)
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

    // List /sdcard and show a few entries (the FileManager UI's browse path).
    std::atomic<bool> list_done{false};
    std::atomic<adb::Error> list_err{adb::Error::Ok};
    size_t list_count = 0;
    std::vector<adb::DirEntry> entries;
    sync->list("/sdcard", [&](adb::Error err, std::vector<adb::DirEntry> es) {
        list_err = err;
        list_count = es.size();
        entries = std::move(es);
        list_done = true;
    });
    for (int i = 0; i < 100 && !list_done; ++i) sleep_ms(100);
    std::printf("list /sdcard: err=%s count=%zu\n",
                adb::to_string(list_err.load()), list_count);
    int shown = 0;
    for (const auto& e : entries) {
        if (e.name == "." || e.name == "..") continue;
        std::printf("    %s %s (%u)\n", e.is_dir() ? "d" : "-", e.name.c_str(),
                    e.size);
        if (++shown >= 8) break;
    }
    bool listed = list_done && list_err.load() == adb::Error::Ok && list_count > 0;

    sync->close();
    sync.reset();   // drop the sync; the weak listener ref expires with it
    client->close();

    bool one_close = listener->closes() == 1;
    bool ok = pushed && landed && listed && one_close;
    std::printf("%s (closes=%d)\n",
                ok ? "PASSED: push lands, list works, and sync closes once"
                   : "FAILED",
                listener->closes());
    return ok ? 0 : 1;
}
