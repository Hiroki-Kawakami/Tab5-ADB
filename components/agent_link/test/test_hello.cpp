// Headless test for the tab5adb-agent HELLO handshake (run on the desktop
// against a real Android device — the Tab5 role, played by the real
// embedded_adb/adb stack over libusb, NO LVGL/SDL). This is testing.md's slices
// A–C end to end: connect_usb -> Sync::push the agent jar -> launch it via a
// Shell (app_process) -> agent_link::Link::open localabstract:tab5adb-agent ->
// answer the agent's HELLO -> verify on_link_hello fires and the agent logs
// "HELLO ok" back over its stdout (the Shell stream).
//
// Prereqs: the phone authorized (run components/adb/test/test_client once and
// tap Allow) and the agent jar built (nix develop -c android-agent/build.sh).
//
// Build & run with the test runner (it compiles everything, runs
// `adb kill-server`, and launches the test):
//   nix develop -c components/agent_link/test/run.sh
// Pass a jar path as argv[1] to override the default (build.sh's output).
// (See test/run.sh for the underlying g++/gcc command if you need to invoke it
// by hand.)
#include "adb.hpp"
#include "agent_link.hpp"

#include <nvs_flash.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kRemoteJar = "/data/local/tmp/tab5adb-agent.jar";
constexpr const char* kLaunchCmd =
    "CLASSPATH=/data/local/tmp/tab5adb-agent.jar app_process / com.tab5adb.agent.Server";

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// One object plays every delegate role (single device), mirroring the other
// test harnesses. All callbacks fire on reader/worker threads; with no LVGL the
// test reads the atomics directly.
class Listener : public adb::ClientListener,
                 public adb::SyncListener,
                 public adb::ShellListener,
                 public agent_link::LinkLifecycleListener {
public:
    // --- ClientListener ---
    void on_state(adb::Client*, adb::ConnectionState s) override {
        std::printf(">>> state: %s\n", adb::to_string(s));
        if (s == adb::ConnectionState::Online) online_ = true;
        if (s == adb::ConnectionState::Unauthorized) {
            std::printf("    -> authorize the device first (run test_client)\n");
        }
    }
    bool online() const { return online_; }

    // --- SyncListener ---
    void on_sync_close(adb::Sync*, adb::Error) override {}

    // --- ShellListener (the agent's stdout/stderr stream back over shell:) ---
    void on_shell_data(adb::Shell*, const uint8_t* d, size_t n) override {
        std::fwrite("agent| ", 1, 7, stdout);
        std::fwrite(d, 1, n, stdout);
        std::fflush(stdout);
        std::string s(reinterpret_cast<const char*>(d), n);
        if (s.find("HELLO ok") != std::string::npos) agent_logged_ok_ = true;
    }
    void on_shell_close(adb::Shell*, adb::Error) override {}

    // --- LinkLifecycleListener ---
    void on_link_hello(agent_link::Link* link, const agent_link::AgentInfo& info) override {
        std::printf(">>> on_link_hello: proto=%u agent=%u.%u.%u caps=0x%04x\n",
                    info.proto_version, info.version_major, info.version_minor,
                    info.version_patch, info.capabilities);
        hello_link_ = link;  // the link that completed HELLO (retries make others)
        hello_ = true;
    }
    void on_link_close(agent_link::Link* link, adb::Error err) override {
        std::printf(">>> on_link_close: %s\n", adb::to_string(err));
        if (link == hello_link_) ++hello_link_closes_;  // exactly-once for THAT link
        link_closed_ = true;
    }

    bool hello() const { return hello_; }
    bool agent_logged_ok() const { return agent_logged_ok_; }
    bool link_closed() const { return link_closed_; }
    int hello_link_closes() const { return hello_link_closes_; }
    void reset_link() { link_closed_ = false; }

private:
    std::atomic<bool> online_{false};
    std::atomic<bool> hello_{false};
    std::atomic<bool> agent_logged_ok_{false};
    std::atomic<bool> link_closed_{false};
    std::atomic<agent_link::Link*> hello_link_{nullptr};
    std::atomic<int> hello_link_closes_{0};
};

// Pull source for Sync::push: serves an in-memory buffer.
struct BufSource {
    const std::string* data;
    size_t off = 0;
    int operator()(uint8_t* buf, size_t cap) {
        size_t n = std::min(cap, data->size() - off);
        std::memcpy(buf, data->data() + off, n);
        off += n;
        return static_cast<int>(n);  // 0 at EOF
    }
};

bool push_jar(const std::shared_ptr<adb::Client>& client,
              const std::shared_ptr<Listener>& listener, const std::string& jar) {
    std::ifstream f(jar, std::ios::binary);
    if (!f) {
        std::printf("FAIL: cannot open jar '%s'\n", jar.c_str());
        return false;
    }
    std::string bytes((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    std::printf("push %s (%zu bytes) -> %s\n", jar.c_str(), bytes.size(), kRemoteJar);

    auto sync = client->open_sync(listener);
    if (!sync) {
        std::printf("FAIL: open_sync returned nullptr\n");
        return false;
    }
    auto src = std::make_shared<BufSource>();
    src->data = &bytes;
    std::atomic<bool> done{false};
    std::atomic<adb::Error> err{adb::Error::Ok};
    sync->push(kRemoteJar, 0644, /*mtime=*/0,
               [src](uint8_t* b, size_t c) { return (*src)(b, c); },
               [&](adb::Error e) { err = e; done = true; });
    for (int i = 0; i < 100 && !done; ++i) sleep_ms(100);
    sync->close();
    bool ok = done && err.load() == adb::Error::Ok;
    if (!ok) std::printf("FAIL: push (%s)\n", adb::to_string(err.load()));
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    nvs_flash_sim_set_path("/tmp/adb_test_nvs.json");
    const std::string jar =
        argc > 1 ? argv[1] : "../../android-agent/build/tab5adb-agent.jar";

    auto listener = std::make_shared<Listener>();  // held weakly downstream
    auto client = adb::Client::connect_usb(listener);

    for (int i = 0; i < 200 && !listener->online(); ++i) {
        if (client->state() == adb::ConnectionState::Closed) break;
        sleep_ms(100);
    }
    if (!listener->online()) {
        std::printf("FAIL: did not reach Online\n");
        return 1;
    }

    // Kill any stale agent so its LocalServerSocket bind doesn't fail, then push.
    std::atomic<bool> kill_done{false};
    client->exec("pkill -f com.tab5adb.agent.Server; true",
                 [&](adb::Error, const std::string&) { kill_done = true; });
    for (int i = 0; i < 30 && !kill_done; ++i) sleep_ms(100);

    if (!push_jar(client, listener, jar)) {
        client->close();
        return 1;
    }

    // Launch the agent under a Shell so its stdout streams back to us.
    std::printf("launch: %s\n", kLaunchCmd);
    auto shell = client->open_shell(listener, kLaunchCmd);
    if (!shell) {
        std::printf("FAIL: open_shell returned nullptr\n");
        client->close();
        return 1;
    }

    // Retry localabstract until the agent is listening (protocol.md §2.2-3): each
    // attempt opens a Link; a quick close without a HELLO = agent not up yet.
    std::shared_ptr<agent_link::Link> link;
    for (int attempt = 0; attempt < 30 && !listener->hello(); ++attempt) {
        listener->reset_link();
        link = agent_link::Link::open(client, listener);
        if (!link) {  // not Online (shouldn't happen here)
            sleep_ms(200);
            continue;
        }
        for (int i = 0; i < 10 && !listener->hello() && !listener->link_closed(); ++i) {
            sleep_ms(100);
        }
        if (listener->hello()) break;
        link.reset();        // drop this attempt's link (rejected / agent not up)
        sleep_ms(300);
    }

    // Give the agent a moment to print "HELLO ok" back over its stdout.
    for (int i = 0; i < 20 && !listener->agent_logged_ok(); ++i) sleep_ms(100);

    bool hello = listener->hello();
    bool agent_ok = listener->agent_logged_ok();
    std::printf("hello=%s agent_logged_ok=%s\n", hello ? "ok" : "FAILED",
                agent_ok ? "ok" : "FAILED");

    // Teardown: close the link, then the shell (kills the agent), then the client.
    if (link) link->close();
    sleep_ms(200);
    shell->close();
    shell.reset();
    link.reset();
    client->close();

    bool one_close = listener->hello_link_closes() == 1;  // the successful link
    bool ok = hello && agent_ok && one_close;
    std::printf("%s (hello_link_closes=%d)\n",
                ok ? "PASSED: HELLO handshake completes both sides, link closes once"
                   : "FAILED",
                listener->hello_link_closes());
    return ok ? 0 : 1;
}
