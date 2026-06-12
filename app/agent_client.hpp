#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "adb.hpp"         // adb::Client/Shell/Sync + listeners, adb::Error
#include "agent_link.hpp"  // agent_link::Link + LinkLifecycleListener

namespace app {

// AgentClient — app-global manager for the tab5adb-agent connection.
//
// Where agent_link::Link is the per-stream protocol engine (framing / HELLO /
// mirror control / video demux), AgentClient owns the AGENT PROCESS LIFECYCLE and
// makes the link a persistent, lazily-started, shared resource decoupled from any
// one screen — the same layering as adb::Client (lifecycle) over AdbConnection
// (protocol). It does NOT forward per-feature protocol; features drive the
// protocol on the Link directly (link()->start_mirror()/stop_mirror(),
// set_video_listener()), so AgentClient never grows per-feature methods.
//
// What it owns that Link does not:
//   - Bring-up: pkill stale agent -> Sync::push the embedded jar -> open_shell
//     app_process -> retry Link::open until the agent answers HELLO. Runs on a
//     private worker task.
//   - Persistence: holds the agent Shell (its stdout) + the Link alive across the
//     transient screens.
//   - A lazy, idempotent connection state machine (Disconnected/Connecting/Ready)
//     with coalesced ensure_connected() callbacks.
//   - Teardown tied to the adb connection (on_adb_disconnected()).
//
// Threading: ensure_connected()'s callback fires on the LVGL thread (like
// app::adb_connect_async) so a feature can drive its waiting UI. The link's
// lifecycle callbacks (HELLO/close) drive the state machine on the adb reader
// thread; the video channel a feature registers on the Link fires there too
// (unchanged from agent_link). state()/ready()/link() are callable from any thread.
//
// Standard feature flow (e.g. the mirror screen):
//   bool warm = app::agent_client().ready();
//   if (!warm) show_waiting();
//   app::agent_client().ensure_connected([self_weak, warm](bool ok) {  // LVGL thread
//       if (!warm) hide_waiting();
//       auto s = self_weak.lock(); if (!s || !ok) { /* error */ return; }
//       auto l = app::agent_client().link();
//       if (!l) return;
//       l->set_video_listener(self_weak);   // VideoListener
//       l->start_mirror({});
//   });
//   // on exit: if (auto l = app::agent_client().link()) { l->stop_mirror();
//   //          l->set_video_listener({}); }  // link stays alive for next time
class AgentClient : public agent_link::LinkLifecycleListener,
                    public adb::SyncListener,
                    public adb::ShellListener,
                    public std::enable_shared_from_this<AgentClient> {
public:
    enum class State { Disconnected, Connecting, Ready };

    // Whether this adb session runs with the agent (Normal) or without it
    // (Limited) — the app's feature gate: Normal offers the agent-backed features
    // (mirroring, the mirror-based preview, app icons), Limited only the plain-adb
    // ones. Determined by the EAGER bring-up the connect flow runs right after the
    // adb link comes Online (HomeScreen waits for it before pushing the device
    // screen), then refined by any later bring-up result; reset to Unknown when
    // the adb link drops. Distinct from State: a Normal-mode agent can be
    // momentarily Disconnected (the link dropped, ensure_connected re-launches).
    enum class Mode { Unknown, Normal, Limited };

    AgentClient() = default;
    ~AgentClient() override = default;

    AgentClient(const AgentClient&) = delete;
    AgentClient& operator=(const AgentClient&) = delete;

    // Lazily bring the agent up (jar push + app_process launch + HELLO) over the
    // app's Online adb::Client. `cb` fires on the LVGL thread: true once Ready,
    // false on failure (e.g. adb not Online, or the agent never answered). If
    // already Ready, `cb` is posted immediately; concurrent calls while Connecting
    // coalesce onto the single in-flight bring-up.
    void ensure_connected(std::function<void(bool ok)> cb);

    State state() const { return state_.load(); }
    bool ready() const { return state_.load() == State::Ready; }
    Mode mode() const { return mode_.load(); }

    // The agent's HELLO capability bits (agent_link::Cap, §4.6) from the current
    // link; 0 until Ready. The embedded jar always matches the firmware, so a
    // missing bit means the agent dropped the feature at runtime (e.g. no
    // PackageManager) — gate optional features (kCapAppInfo) on this.
    uint16_t agent_caps() const { return agent_caps_.load(); }

    // The established link, or nullptr unless Ready. Drive the protocol on it
    // directly (start_mirror/stop_mirror/set_video_listener). Do NOT close() it —
    // AgentClient owns the link's lifetime. Returned as a shared_ptr so the caller
    // holds it alive across the call even if the link drops concurrently.
    std::shared_ptr<agent_link::Link> link();

    // Called by the app's adb holder when the adb connection drops: tear the agent
    // link/shell down and go Disconnected (a later ensure_connected re-launches).
    void on_adb_disconnected();

    // --- agent_link::LinkLifecycleListener (adb reader thread) ---
    void on_link_hello(agent_link::Link* link, const agent_link::AgentInfo& info) override;
    void on_link_close(agent_link::Link* link, adb::Error err) override;

    // --- adb::SyncListener / ShellListener (jar push + agent stdout) ---
    void on_sync_close(adb::Sync*, adb::Error) override {}
    void on_shell_data(adb::Shell*, const uint8_t* d, size_t n) override;
    void on_shell_close(adb::Shell*, adb::Error) override;

private:
    static void trampoline(void* arg);
    void run();                  // worker task body: the bring-up sequence
    bool push_jar();             // Sync::push the embedded agent jar
    void finish_result(bool ok);  // set the final state + fire waiters
    void reset_session();         // drop link/shell/sync/client (off any callback stack)

    void reset_worker_flags_locked();
    bool stopping();
    bool shell_dead();
    void sleep_ms(int ms);
    template <class Pred>
    void wait_for(Pred pred, int ms);

    std::atomic<State> state_{State::Disconnected};
    std::atomic<Mode> mode_{Mode::Unknown};
    std::atomic<uint16_t> agent_caps_{0};

    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::function<void(bool)>> waiters_;  // pending ensure_connected cbs

    // Worker step flags (guarded by mtx_, waited on cv_).
    bool stop_ = false;
    bool kill_done_ = false;
    bool push_done_ = false;
    adb::Error push_err_ = adb::Error::Transport;
    bool hello_ = false;
    bool link_closed_ = false;
    bool shell_closed_ = false;  // the agent's app_process exited (launch failure)

    // Session objects (guarded by mtx_). Kept alive while Ready.
    std::shared_ptr<adb::Client> client_;
    std::shared_ptr<adb::Shell> shell_;
    std::shared_ptr<adb::Sync> sync_;
    std::shared_ptr<agent_link::Link> link_;
};

// The app-global AgentClient (created on first use).
AgentClient& agent_client();

}  // namespace app
