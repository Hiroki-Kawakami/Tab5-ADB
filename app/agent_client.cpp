#include "agent_client.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "adb_app.hpp"          // app::adb_client_shared()
#include "agent/agent_jar.h"    // agent_jar, agent_jar_len
#include "lvgl.hpp"             // lv_async_call(std::function<void()>)

namespace app {

namespace {

constexpr const char* kRemoteJar = "/data/local/tmp/tab5adb-agent.jar";
// Launch the agent with app_process (shell uid 2000, reaches hidden APIs); no
// --test-pattern, so it mirrors the live screen via SurfaceControl capture.
constexpr const char* kLaunchCmd =
    "CLASSPATH=/data/local/tmp/tab5adb-agent.jar app_process / com.tab5adb.agent.Server";
constexpr const char* kKillCmd = "pkill -f com.tab5adb.agent.Server; true";

void post_result(std::function<void(bool)> cb, bool ok) {
    if (!cb) return;
    lv_async_call([cb = std::move(cb), ok]() { cb(ok); });
}

}  // namespace

AgentClient& agent_client() {
    // Intentionally leaked (never destroyed): the singleton must outlive every
    // other static whose destructor calls into it at process exit — e.g.
    // ScreenManager destroying a live ADBMirroringScreen, whose dtor calls
    // agent_client().link(). Cross-TU static destruction order is unspecified, so a
    // normal static would race; the leaked heap shared_ptr keeps the controlling
    // ref (so shared_from_this works) and lives for the whole process. The device
    // firmware never exits, so nothing is actually leaked there.
    static std::shared_ptr<AgentClient>* inst =
        new std::shared_ptr<AgentClient>(std::make_shared<AgentClient>());
    return **inst;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void AgentClient::ensure_connected(std::function<void(bool)> cb) {
    std::function<void(bool)> fire_now;     // a single cb to satisfy immediately
    bool fire_now_ok = false;
    std::vector<std::function<void(bool)>> fail_now;  // cbs to fail immediately
    bool spawn = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        State s = state_.load();
        if (s == State::Ready) {
            fire_now = std::move(cb);
            fire_now_ok = true;
        } else {
            waiters_.push_back(std::move(cb));
            if (s == State::Disconnected) {
                auto client = app::adb_client_shared();
                if (!client || client->state() != adb::ConnectionState::Online) {
                    fail_now.swap(waiters_);  // can't bring the agent up: fail all
                } else {
                    client_ = std::move(client);
                    reset_worker_flags_locked();
                    state_.store(State::Connecting);
                    spawn = true;
                }
            }
            // Connecting: the in-flight worker will fire this waiter.
        }
    }
    if (fire_now) post_result(std::move(fire_now), fire_now_ok);
    for (auto& f : fail_now) post_result(std::move(f), false);
    if (spawn) {
        // The singleton outlives the task, so pass the raw `this` (no join needed).
        xTaskCreate(&AgentClient::trampoline, "agent_conn", 8192, this, 5, nullptr);
    }
}

std::shared_ptr<agent_link::Link> AgentClient::link() {
    std::lock_guard<std::mutex> lk(mtx_);
    return link_;
}

void AgentClient::on_adb_disconnected() {
    bool reset = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;  // signal any running worker to bail
        if (state_.load() == State::Ready) {
            state_.store(State::Disconnected);
            reset = true;
        }
    }
    cv_.notify_all();
    // A Connecting worker bails on stop_ and resets via finish_result. When Ready,
    // drop the session objects off any callback stack (the LVGL thread).
    if (reset) {
        auto self = shared_from_this();
        lv_async_call([self]() { self->reset_session(); });
    }
}

// ---------------------------------------------------------------------------
// agent_link::LinkLifecycleListener (adb reader thread)
// ---------------------------------------------------------------------------

void AgentClient::on_link_hello(agent_link::Link*, const agent_link::AgentInfo&) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        hello_ = true;
    }
    cv_.notify_all();
}

void AgentClient::on_link_close(agent_link::Link*, adb::Error err) {
    bool teardown = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        link_closed_ = true;  // the worker's retry loop watches this
        // Only an established (Ready) link dropping is a session teardown; closes
        // of the worker's failed retry attempts (Connecting) are expected.
        if (state_.load() == State::Ready) {
            state_.store(State::Disconnected);
            teardown = true;
        }
    }
    cv_.notify_all();
    if (teardown) {
        std::printf("agent: link closed (%s) -> disconnected\n", adb::to_string(err));
        std::fflush(stdout);
        // Drop the session objects off the Link's own callback stack.
        auto self = shared_from_this();
        lv_async_call([self]() { self->reset_session(); });
    }
}

// ---------------------------------------------------------------------------
// adb::ShellListener — the agent's stdout/stderr stream back over shell:
// ---------------------------------------------------------------------------

void AgentClient::on_shell_data(adb::Shell*, const uint8_t* d, size_t n) {
    std::fwrite("agent| ", 1, 7, stdout);
    std::fwrite(d, 1, n, stdout);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// Worker task — the bring-up sequence (protocol.md §2.2)
// ---------------------------------------------------------------------------

void AgentClient::trampoline(void* arg) {
    static_cast<AgentClient*>(arg)->run();
    vTaskDelete(nullptr);
}

void AgentClient::run() {
    // 1. Kill any stale agent from a previous session (frees its socket bind).
    std::printf("agent: stopping previous agent...\n");
    client_->exec(kKillCmd, [w = weak_from_this()](adb::Error, const std::string&) {
        if (auto s = w.lock()) {
            std::lock_guard<std::mutex> lk(s->mtx_);
            s->kill_done_ = true;
            s->cv_.notify_all();
        }
    });
    wait_for([this] { return kill_done_; }, 3000);
    if (stopping()) return finish_result(false);

    // 2. Push the embedded agent jar.
    std::printf("agent: pushing jar...\n");
    if (!push_jar()) return finish_result(false);
    if (stopping()) return finish_result(false);

    // 3. Launch it with app_process; its stdout/stderr stream over this shell,
    //    which AgentClient holds open for the agent's lifetime.
    std::printf("agent: launching app_process...\n");
    {
        auto shell = client_->open_shell(weak_from_this(), kLaunchCmd);
        std::lock_guard<std::mutex> lk(mtx_);
        shell_ = std::move(shell);
    }

    // 4. Open the link, retrying until the agent answers HELLO (protocol.md §2.2).
    std::printf("agent: connecting localabstract...\n");
    std::shared_ptr<agent_link::Link> link;
    for (int attempt = 0; attempt < 40 && !hello_ && !stopping(); ++attempt) {
        { std::lock_guard<std::mutex> lk(mtx_); link_closed_ = false; }
        link = agent_link::Link::open(client_, weak_from_this());
        if (!link) { sleep_ms(200); continue; }
        { std::lock_guard<std::mutex> lk(mtx_); link_ = link; }
        wait_for([this] { return hello_ || link_closed_; }, 1500);
        if (hello_) break;
        { std::lock_guard<std::mutex> lk(mtx_); link_.reset(); }  // detach this attempt
        link->close();
        sleep_ms(300);
    }

    bool ok = hello_ && !stopping();
    if (!ok) std::printf("agent: bring-up failed\n");
    else std::printf("agent: ready\n");
    finish_result(ok);
}

bool AgentClient::push_jar() {
    {
        auto sync = client_->open_sync(weak_from_this());
        std::lock_guard<std::mutex> lk(mtx_);
        sync_ = std::move(sync);
    }
    if (!sync_) return false;

    auto off = std::make_shared<size_t>(0);
    { std::lock_guard<std::mutex> lk(mtx_); push_done_ = false; }
    sync_->push(
        kRemoteJar, 0644, /*mtime=*/0,
        [off](uint8_t* b, size_t cap) -> int {
            size_t rem = agent_jar_len - *off;
            size_t n = std::min(cap, rem);
            std::memcpy(b, agent_jar + *off, n);
            *off += n;
            return static_cast<int>(n);  // 0 at EOF
        },
        [w = weak_from_this()](adb::Error e) {
            if (auto s = w.lock()) {
                std::lock_guard<std::mutex> lk(s->mtx_);
                s->push_err_ = e;
                s->push_done_ = true;
                s->cv_.notify_all();
            }
        });
    wait_for([this] { return push_done_; }, 15000);

    bool ok;
    std::shared_ptr<adb::Sync> sync;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ok = push_done_ && push_err_ == adb::Error::Ok;
        sync = sync_;
        sync_.reset();
    }
    if (sync) sync->close();  // the sync: session is one-shot for the push
    return ok;
}

void AgentClient::finish_result(bool ok) {
    std::vector<std::function<void(bool)>> waiters;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_.store(ok ? State::Ready : State::Disconnected);
        if (!ok) {
            link_.reset();
            shell_.reset();
            sync_.reset();
            client_.reset();
        }
        waiters.swap(waiters_);
    }
    for (auto& f : waiters) post_result(std::move(f), ok);
}

void AgentClient::reset_session() {
    std::shared_ptr<agent_link::Link> link;
    std::shared_ptr<adb::Shell> shell;
    std::shared_ptr<adb::Sync> sync;
    std::shared_ptr<adb::Client> client;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        link = std::move(link_);
        shell = std::move(shell_);
        sync = std::move(sync_);
        client = std::move(client_);
    }
    // Released here (outside the lock): dropping shell_ closes the agent's shell
    // stream = the agent process exits; dropping link_ closes its stream.
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

void AgentClient::reset_worker_flags_locked() {
    stop_ = false;
    hello_ = false;
    link_closed_ = false;
    kill_done_ = false;
    push_done_ = false;
    push_err_ = adb::Error::Transport;
}

bool AgentClient::stopping() {
    std::lock_guard<std::mutex> lk(mtx_);
    return stop_;
}

void AgentClient::sleep_ms(int ms) { wait_for([] { return false; }, ms); }

template <class Pred>
void AgentClient::wait_for(Pred pred, int ms) {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait_for(lk, std::chrono::milliseconds(ms),
                 [&] { return stop_ || pred(); });
}

}  // namespace app
