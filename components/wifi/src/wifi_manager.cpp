// wifi::Manager — portable state machine over a per-target Backend.
//
// Owns: the leaked singleton, one-shot connect/scan completion bookkeeping, the
// connect timeout (a FreeRTOS one-shot software timer), NVS persistence of the
// last network, the Status snapshot, and Listener dispatch. Callbacks are fired
// OUTSIDE the lock so the app may call back in (e.g. status()) without deadlock.
#include "wifi_manager.hpp"

#include <mutex>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wifi_backend.hpp"

namespace wifi {

static const char* TAG = "wifi";
static constexpr char kNvsNs[] = "wifi";

namespace {
// A unit of blocking radio work for the Manager's worker task. Enqueued as a
// heap pointer (the queue copies the pointer); the worker deletes it.
enum class Op { Enable, Disable, Autoconnect };
struct WorkItem {
    Op op;
    int timeout_ms;             // Autoconnect
    std::function<void()> done; // optional completion (fires on the worker thread)
};
}  // namespace

const char* result_str(Result r) {
    switch (r) {
        case Result::Ok:          return "Ok";
        case Result::ApNotFound:  return "ApNotFound";
        case Result::AuthFailed:  return "AuthFailed";
        case Result::AssocFailed: return "AssocFailed";
        case Result::IpFailed:    return "IpFailed";
        case Result::Timeout:     return "Timeout";
        case Result::Failed:      return "Failed";
    }
    return "?";
}

struct Manager::Impl : BackendHost {
    std::mutex mtx;
    Backend* backend = nullptr;
    bool started = false;

    QueueHandle_t cmd_q = nullptr;   // WorkItem* queue drained by the worker task
    TaskHandle_t worker = nullptr;

    std::weak_ptr<Listener> listener;

    State state = State::Off;
    std::string ssid;   // current target / connected SSID
    std::string ip;     // valid when Connected

    bool connecting = false;     // a connect() is in flight
    ConnectCb connect_cb;        // pending one-shot connect completion
    ScanCb scan_cb;              // pending one-shot scan completion
    TimerHandle_t timeout_timer = nullptr;

    // ---- helpers (call with mtx held) ----
    Status snapshot_locked() const {
        Status s;
        s.state = state;
        s.ssid = ssid;
        s.ip = (state == State::Connected) ? ip : std::string();
        s.rssi = (state == State::Connected && backend) ? backend->rssi() : 0;
        return s;
    }

    // Dispatch the persistent listener with the current snapshot. Must be called
    // WITHOUT mtx held (locks the weak listener, then invokes app code).
    void notify(const Status& s) {
        if (auto l = listener.lock()) l->on_wifi_state(s);
    }

    void arm_timeout(int ms) {
        if (!timeout_timer) return;
        xTimerChangePeriod(timeout_timer, pdMS_TO_TICKS(ms), 0);
        xTimerStart(timeout_timer, 0);
    }
    void cancel_timeout() {
        if (timeout_timer) xTimerStop(timeout_timer, 0);
    }

    // ---- BackendHost (event thread) ----
    void on_scan_result(Result r, std::vector<AP> aps) override {
        ScanCb cb;
        {
            std::lock_guard<std::mutex> lk(mtx);
            cb = std::move(scan_cb);
            scan_cb = nullptr;
        }
        if (cb) cb(r, std::move(aps));
    }

    void on_associated() override {
        // Associated but no IP yet — stay Connecting. No listener edge (the
        // user-visible transition is Connected on got_ip).
    }

    void on_got_ip(const std::string& got_ip) override {
        ConnectCb cb;
        Status s;
        {
            std::lock_guard<std::mutex> lk(mtx);
            ip = got_ip;
            state = State::Connected;
            cancel_timeout();
            if (connecting) {
                connecting = false;
                cb = std::move(connect_cb);
                connect_cb = nullptr;
            }
            s = snapshot_locked();
        }
        ESP_LOGI(TAG, "connected: %s ip=%s", s.ssid.c_str(), s.ip.c_str());
        if (cb) cb(Result::Ok);
        notify(s);
    }

    void on_disconnected(Result reason) override {
        ConnectCb cb;
        Status s;
        bool was_connecting;
        {
            std::lock_guard<std::mutex> lk(mtx);
            was_connecting = connecting;
            ip.clear();
            // Stay Off if the radio was turned off (esp_wifi_stop surfaces a
            // disconnect event we must not read as a drop-to-idle).
            if (state != State::Off) state = State::Disconnected;
            cancel_timeout();
            if (connecting) {
                connecting = false;
                cb = std::move(connect_cb);
                connect_cb = nullptr;
            }
            s = snapshot_locked();
        }
        if (was_connecting)
            ESP_LOGW(TAG, "connect failed: %s", result_str(reason));
        else
            ESP_LOGW(TAG, "link dropped");
        if (cb) cb(reason);
        notify(s);
    }

    // Timeout timer trampoline (FreeRTOS timer task).
    static void timeout_cb(TimerHandle_t t) {
        auto* self = static_cast<Impl*>(pvTimerGetTimerID(t));
        bool fire;
        {
            std::lock_guard<std::mutex> lk(self->mtx);
            fire = self->connecting;
        }
        if (fire && self->backend) self->backend->disconnect();
        // The disconnect() will surface as on_disconnected; but esp_wifi may not
        // emit one if it never associated, so complete here too (idempotent: the
        // first of the two to clear `connecting` wins).
        ConnectCb cb;
        Status s;
        {
            std::lock_guard<std::mutex> lk(self->mtx);
            if (!self->connecting) return;  // already completed
            self->connecting = false;
            self->state = State::Disconnected;
            cb = std::move(self->connect_cb);
            self->connect_cb = nullptr;
            s = self->snapshot_locked();
        }
        ESP_LOGW(TAG, "connect timed out");
        if (cb) cb(Result::Timeout);
        self->notify(s);
    }

    // ---- NVS persistence ----
    bool load_creds(std::string& out_ssid, std::string& out_pass) const {
        nvs_handle_t h;
        if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return false;
        char buf[128];
        size_t len = sizeof(buf);
        bool ok = nvs_get_str(h, "ssid", buf, &len) == ESP_OK && buf[0];
        if (ok) out_ssid.assign(buf);
        if (ok) {
            len = sizeof(buf);
            out_pass = (nvs_get_str(h, "pass", buf, &len) == ESP_OK) ? buf : "";
        }
        nvs_close(h);
        return ok;
    }
    void save_creds(const std::string& s, const std::string& p) {
        nvs_handle_t h;
        if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
        nvs_set_str(h, "ssid", s.c_str());
        nvs_set_str(h, "pass", p.c_str());
        nvs_commit(h);
        nvs_close(h);
    }
    void erase_creds() {
        nvs_handle_t h;
        if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
};

Manager::Manager() : p_(new Impl) {
    // One worker task + command queue serialize the blocking radio operations
    // (bring-up, on/off, autoconnect) so nothing races bring_up() and the LVGL
    // thread never blocks. The Manager is a leaked singleton, so the task lives
    // for the process lifetime (no join).
    p_->cmd_q = xQueueCreate(8, sizeof(WorkItem*));
    xTaskCreate(&Manager::worker_trampoline, "wifi_mgr", 8192, this, 4, &p_->worker);
}

void Manager::worker_trampoline(void* arg) { static_cast<Manager*>(arg)->worker_loop(); }

void Manager::worker_loop() {
    for (;;) {
        WorkItem* it = nullptr;
        if (xQueueReceive(p_->cmd_q, &it, portMAX_DELAY) != pdTRUE || !it) continue;
        switch (it->op) {
            case Op::Enable:      bring_up(); apply_enabled(true);  break;
            case Op::Disable:     apply_enabled(false);             break;
            case Op::Autoconnect: bring_up(); do_autoconnect(it->timeout_ms); break;
        }
        if (it->done) it->done();
        delete it;
    }
}

// Enqueue a WorkItem; drops (and runs done) if the queue is unexpectedly full so a
// caller waiting on `done` is never stuck.
static void enqueue(QueueHandle_t q, WorkItem* it) {
    if (!q || xQueueSend(q, &it, 0) != pdTRUE) {
        if (it->done) it->done();
        delete it;
    }
}

void Manager::set_listener(std::weak_ptr<Listener> listener) {
    std::lock_guard<std::mutex> lk(p_->mtx);
    p_->listener = std::move(listener);
}

void Manager::start(std::weak_ptr<Listener> listener) {
    set_listener(std::move(listener));
    enqueue(p_->cmd_q, new WorkItem{Op::Enable, 0, {}});
}

void Manager::bring_up() {
    bool first;
    {
        std::lock_guard<std::mutex> lk(p_->mtx);
        first = !p_->started;
        p_->started = true;
    }
    if (!first) return;

    // esp_wifi needs NVS; ensure it (idempotent across the app's consumers).
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    p_->timeout_timer = xTimerCreate("wifi_to", pdMS_TO_TICKS(15000), pdFALSE,
                                     p_.get(), &Impl::timeout_cb);
    p_->backend = make_backend(p_.get());
    p_->backend->start();
    {
        std::lock_guard<std::mutex> lk(p_->mtx);
        if (p_->state == State::Off) p_->state = State::Disconnected;
    }
}

void Manager::autoconnect_saved(int timeout_ms) {
    enqueue(p_->cmd_q, new WorkItem{Op::Autoconnect, timeout_ms, {}});
}

// Worker thread (after bring_up). Connect to the saved creds if any.
void Manager::do_autoconnect(int timeout_ms) {
    std::string s, pw;
    if (!p_->load_creds(s, pw)) {
        ESP_LOGI(TAG, "autoconnect: no saved network");
        return;
    }
    ESP_LOGI(TAG, "autoconnect: connecting to %s", s.c_str());
    connect(s, pw, [](Result) {}, timeout_ms);  // status flows via the Listener
}

bool Manager::configured() const {
    std::string s, pw;
    return p_->load_creds(s, pw);
}

std::string Manager::saved_ssid() const {
    std::string s, pw;
    return p_->load_creds(s, pw) ? s : std::string();
}

Status Manager::status() const {
    std::lock_guard<std::mutex> lk(p_->mtx);
    return p_->snapshot_locked();
}

void Manager::scan(ScanCb cb) {
    ScanCb superseded;
    {
        std::lock_guard<std::mutex> lk(p_->mtx);
        if (!p_->backend) { /* not started */ }
        superseded = std::move(p_->scan_cb);
        p_->scan_cb = std::move(cb);
    }
    if (superseded) superseded(Result::Failed, {});
    if (p_->backend) p_->backend->scan();
    else p_->on_scan_result(Result::Failed, {});
}

void Manager::connect(const std::string& ssid, const std::string& password,
                      ConnectCb cb, int timeout_ms) {
    ConnectCb superseded;
    {
        std::lock_guard<std::mutex> lk(p_->mtx);
        if (!p_->backend) {
            // not started — fail synchronously below
        }
        superseded = std::move(p_->connect_cb);
        p_->connect_cb = std::move(cb);
        p_->connecting = true;
        p_->ssid = ssid;
        p_->ip.clear();
        p_->state = State::Connecting;
    }
    if (superseded) superseded(Result::Failed);

    Status s;
    { std::lock_guard<std::mutex> lk(p_->mtx); s = p_->snapshot_locked(); }
    p_->notify(s);

    if (!p_->backend) { p_->on_disconnected(Result::Failed); return; }

    // Persist eagerly so connect_saved()/the UI knows the last attempt; on a
    // failed connect the creds simply won't connect next time (harmless).
    p_->save_creds(ssid, password);
    p_->arm_timeout(timeout_ms);
    p_->backend->connect(ssid, password);
}

void Manager::connect_saved(ConnectCb cb, int timeout_ms) {
    std::string s, pw;
    if (!p_->load_creds(s, pw)) { cb(Result::Failed); return; }
    connect(s, pw, std::move(cb), timeout_ms);
}

void Manager::set_enabled(bool on, std::function<void()> done) {
    enqueue(p_->cmd_q, new WorkItem{on ? Op::Enable : Op::Disable, 0, std::move(done)});
}

// Worker thread. Apply the radio on/off state transition (Enable runs bring_up()
// first, so the backend exists).
void Manager::apply_enabled(bool on) {
    ConnectCb pending;
    Status s;
    {
        std::lock_guard<std::mutex> lk(p_->mtx);
        if (!p_->backend) return;  // bring_up() did not run (should not happen)
        bool is_off = (p_->state == State::Off);
        if (on == !is_off) return;  // already in the requested state
        if (on) {
            p_->state = State::Disconnected;
        } else {
            p_->cancel_timeout();
            if (p_->connecting) {
                p_->connecting = false;
                pending = std::move(p_->connect_cb);
                p_->connect_cb = nullptr;
            }
            p_->ip.clear();
            p_->ssid.clear();
            p_->state = State::Off;
        }
        s = p_->snapshot_locked();
    }
    if (on) p_->backend->radio_on();
    else p_->backend->radio_off();
    if (pending) pending(Result::Failed);  // in-flight connect cancelled by Off
    p_->notify(s);
}

bool Manager::enabled() const {
    std::lock_guard<std::mutex> lk(p_->mtx);
    return p_->state != State::Off;
}

void Manager::disconnect() {
    Status s;
    {
        std::lock_guard<std::mutex> lk(p_->mtx);
        p_->cancel_timeout();
        p_->connecting = false;
        p_->connect_cb = nullptr;
        p_->ip.clear();
        p_->state = State::Disconnected;
        s = p_->snapshot_locked();
    }
    if (p_->backend) p_->backend->disconnect();
    p_->notify(s);
}

void Manager::forget() { p_->erase_creds(); }

Manager& manager() {
    // Deliberately leaked (see header): never destroyed, so exit-time static
    // destructors that call in don't race a destroyed mutex.
    static Manager* m = new Manager();
    return *m;
}

}  // namespace wifi
