// Headless test for adb::Shell (run on the desktop against a real Android
// device). Exercises slice 3: connect_usb() -> open_shell() -> write() ->
// on_shell_data -> close() -> on_shell_close. The reader + writer tasks run on
// the pthread-backed FreeRTOS compat, so this links the idf_compat freertos
// sources too.
//
// The phone must already be authorized (run test_client once and tap Allow).
//
// Build & run with the test runner (builds everything, runs `adb kill-server`,
// launches the test):
//   TEST=test_shell nix develop -c components/adb/test/run.sh
// (See test/run.sh for the underlying g++/gcc command if you need it by hand.)
#include "adb.hpp"

#include <nvs_flash.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

class Listener : public adb::ClientListener, public adb::ShellListener {
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

    // --- ShellListener ---
    void on_shell_data(adb::Shell*, const uint8_t* d, size_t n) override {
        std::lock_guard<std::mutex> lk(m_);
        out_.append(reinterpret_cast<const char*>(d), n);
    }
    void on_shell_close(adb::Shell*, adb::Error err) override {
        std::printf(">>> shell closed (%s)\n", adb::to_string(err));
        ++closes_;
        closed_ = true;
    }
    std::string output() {
        std::lock_guard<std::mutex> lk(m_);
        return out_;
    }
    bool closed() const { return closed_; }
    int closes() const { return closes_; }

private:
    std::atomic<bool> online_{false};
    std::atomic<bool> closed_{false};
    std::atomic<int> closes_{0};
    std::mutex m_;
    std::string out_;
};

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

int main() {
    nvs_flash_sim_set_path("/tmp/adb_test_nvs.json");

    auto listener = std::make_shared<Listener>();  // held weakly by Client/Shell
    auto client = adb::Client::connect_usb(listener);

    for (int i = 0; i < 200 && !listener->online(); ++i) {
        if (client->state() == adb::ConnectionState::Closed) break;
        sleep_ms(100);
    }
    if (!listener->online()) {
        std::printf("FAIL: did not reach Online\n");
        return 1;
    }

    // Open an interactive PTY shell and drive it with a couple of commands.
    auto shell = client->open_shell(listener);
    if (!shell) {
        std::printf("FAIL: open_shell returned nullptr\n");
        return 1;
    }
    shell->write("echo hello from tab5\n");
    shell->write("id\n");
    shell->write("exit\n");  // ends the PTY shell -> device closes the stream

    // Wait for the device to close the stream after `exit` (fall back to an
    // explicit close() if it lingers).
    for (int i = 0; i < 50 && !listener->closed(); ++i) sleep_ms(100);
    shell->close();  // idempotent; no-op if the peer already closed

    std::string out = listener->output();
    std::printf("\n--- shell output ---\n%s--------------------\n", out.c_str());

    bool got_output = out.find("hello from tab5") != std::string::npos;
    bool one_close = listener->closes() == 1;

    shell.reset();   // drop the shell; the weak listener ref expires with it
    client->close();

    bool ok = got_output && one_close;
    std::printf("%s (output=%d, closes=%d)\n",
                ok ? "PASSED: shell streams I/O and closes exactly once"
                   : "FAILED",
                got_output, listener->closes());
    return ok ? 0 : 1;
}
