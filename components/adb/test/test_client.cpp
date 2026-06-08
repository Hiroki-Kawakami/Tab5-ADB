// Headless test for adb::Client (run on the desktop against a real Android
// device). Exercises the first slice: connect_usb() -> state()/banner() ->
// close(). The reader task runs on the pthread-backed FreeRTOS compat, so this
// links the idf_compat freertos sources too.
//
// First run with a fresh key: the phone shows "Allow USB debugging?" — tap Allow.
//
// Build & run with the test runner (builds everything, runs `adb kill-server`,
// launches the test). TEST selects which test/<TEST>.cpp to run:
//   nix develop -c components/adb/test/run.sh              # this test (default)
// (See test/run.sh for the underlying g++/gcc command if you need it by hand.)
#include "adb.hpp"

#include <nvs_flash.h>

#include <chrono>
#include <cstdio>
#include <memory>
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

    auto listener = std::make_shared<Listener>();  // Client holds it weakly
    auto client = adb::Client::connect_usb(listener);

    // Poll for Online (the reader task drives state asynchronously). ~20s budget
    // covers a first-run authorization tap.
    for (int i = 0; i < 200 && !listener->online(); ++i) {
        if (client->state() == adb::ConnectionState::Closed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    bool ok = listener->online();
    std::printf("state() = %s, banner empty = %s\n",
                adb::to_string(client->state()), client->banner().empty() ? "yes" : "no");

    std::printf("closing...\n");
    client->close();  // must return promptly (reader task joined)
    std::printf("closed, state() = %s\n", adb::to_string(client->state()));

    std::printf("%s\n", ok ? "PASSED: reached Online and closed cleanly"
                           : "ended without Online");
    return ok ? 0 : 1;
}
