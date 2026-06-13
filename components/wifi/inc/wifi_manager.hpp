// wifi::Manager — the app-facing Wi-Fi STA connection manager.
//
// Where the BSP covers board hardware and `adb` owns the USB ADB link, this
// component owns the **Wi-Fi station lifecycle**: bringing up the network stack
// (esp_netif / esp_event / esp_wifi on device, a deterministic fake in the
// simulator), scanning, connecting, persisting the last network, and reporting
// connection state. It exists so the Tab5 can join a LAN — the prerequisite for
// the eventual ADB-over-TCP transport (which is an independent `embedded_adb`
// concern: a plain socket once an IP is up).
//
// Layering: this is the single device/simulator split inside the component — the
// esp_wifi API is too large to reimplement on the host (the same call the project
// already makes for embedded_adb's usb_host transport), so the backend is
// ESP_PLATFORM-branched (backend_espwifi.cpp vs backend_sim.cpp) rather than an
// idf_compat esp_wifi shim. The simulator backend is a scriptable fake so
// simverify stays deterministic (no real host-network access).
//
// Threading (same contract as `adb`): all callbacks — one-shot completions AND
// the persistent Listener — fire on the backend's event thread, NOT the LVGL
// thread. Marshalling to LVGL is the app's job (lv_async_call). Query methods
// (status/configured/...) are callable from any thread. The Listener is held as a
// weak_ptr: drop its shared_ptr to detach (no detach() call), Shell/Sync-style.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wifi {

// STA connection state (deduplicated; reported via Listener::on_wifi_state).
enum class State {
    Off,           // start() not called / radio not up
    Disconnected,  // up, not associated (idle, or after a drop)
    Connecting,    // connect in flight (associating / awaiting IP)
    Connected,     // associated AND got an IP (usable)
};

// One-shot result for connect()/scan(). Granularity kept from the reference
// NetworkManager, plus Timeout.
enum class Result {
    Ok,
    ApNotFound,   // SSID not found in range
    AuthFailed,   // wrong password / auth or handshake failure
    AssocFailed,  // association failed
    IpFailed,     // associated but DHCP/IP acquisition failed
    Timeout,      // no terminal event within the connect deadline
    Failed,       // anything else (incl. a superseded/cancelled attempt)
};

const char* result_str(Result r);  // for logs / UI

struct AP {
    std::string ssid;
    int8_t rssi;    // dBm (negative)
    bool secured;   // authmode != OPEN
};

// A snapshot of the current connection (status() — any thread).
struct Status {
    State state = State::Off;
    std::string ssid;  // the connected/associating SSID (empty when idle)
    std::string ip;    // dotted IPv4, valid only when Connected
    int8_t rssi = 0;   // current AP RSSI when Connected, else 0
};

// Persistent connection-state delegate (held weakly). Fires on the event thread.
class Listener {
public:
    virtual ~Listener() = default;
    virtual void on_wifi_state(const Status& status) = 0;
};

class Manager {
public:
    // The radio bring-up (esp_wifi over the SDIO C6 link) and on/off are blocking,
    // so the Manager owns ONE worker task and a command queue: set_enabled() /
    // autoconnect_saved() / start() are NON-BLOCKING — they enqueue work that the
    // worker runs serially (no concurrent bring-up race, never on the LVGL thread).
    // Listener / one-shot callbacks still fire on the backend event thread; a
    // set_enabled() done callback fires on the worker thread. Marshalling to LVGL
    // is the app's job either way.

    // Register the persistent listener + bring the radio up (On) and rejoin the
    // saved network (same path as set_enabled(true)). Idempotent. The work is
    // enqueued to the worker, so this returns immediately. On device the bring-up
    // is where esp_netif/esp_event/esp_wifi_init/esp_wifi_start happen.
    void start(std::weak_ptr<Listener> listener);

    // Register/replace the persistent listener only — cheap and synchronous, no
    // bring-up. For a screen that wants live status without enabling the radio
    // (that comes via start() / set_enabled(true) / autoconnect_saved()). Held
    // weakly: drop the shared_ptr to detach.
    void set_listener(std::weak_ptr<Listener> listener);

    // Boot-time auto-connect: enqueue "bring the radio up (On) and connect to the
    // saved network if one exists" onto the worker. Non-blocking. Does NOT change
    // the registered listener — status flows to whoever is registered.
    void autoconnect_saved(int timeout_ms = 15000);

    // A saved network exists (from a prior successful connect()).
    bool configured() const;
    std::string saved_ssid() const;

    // Current connection snapshot. Callable from any thread.
    Status status() const;

    // STA interface MAC ("aa:bb:cc:dd:ee:ff"), or empty if the radio is not up
    // yet. A device property (does not change with the connection). Any thread.
    std::string mac_address() const;

    // One-shot scan for nearby APs (deduped by SSID, strongest RSSI kept, sorted
    // by RSSI desc). cb fires once on the event thread. A second scan before the
    // first completes supersedes it (the earlier cb fires with an empty list).
    using ScanCb = std::function<void(Result, std::vector<AP>)>;
    void scan(ScanCb cb);

    // One-shot connect. Saves the credentials to NVS on success. cb fires exactly
    // once on the event thread with the outcome. A later steady-state drop is NOT
    // reported through cb (that one already fired) — it arrives via the Listener.
    using ConnectCb = std::function<void(Result)>;
    void connect(const std::string& ssid, const std::string& password, ConnectCb cb,
                 int timeout_ms = 15000);

    // Reconnect using the NVS-saved credentials. cb fires NotConfigured->Failed
    // synchronously if none are saved.
    void connect_saved(ConnectCb cb, int timeout_ms = 15000);

    // Turn the radio on/off (the Wi-Fi enable switch). Non-blocking: the blocking
    // work runs on the worker. Off drops any live/in-flight connection and moves
    // to State::Off (no scanning/connecting until back On); On brings the stack up
    // AND rejoins the saved network (phone-like — same connect path as boot's
    // autoconnect_saved(); a no-op connect if none is saved). Notifies the Listener
    // with the new state; a pending connect() completes (Failed) when turned Off.
    // `done` (optional) fires on the worker thread once the op has been applied
    // (always fires, even for a no-op toggle).
    void set_enabled(bool on, std::function<void()> done = {});
    bool enabled() const;  // false == State::Off (radio down)

    // Disconnect from the current AP (stays up / idle). Notifies the Listener.
    void disconnect();

    // Forget the saved network (clears NVS; does not disconnect a live link).
    void forget();

    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

private:
    friend Manager& manager();
    Manager();
    void bring_up();              // idempotent stack+radio bring-up (worker only)
    void apply_enabled(bool on);  // radio on/off state transition (worker only)
    void do_autoconnect(int timeout_ms);  // connect to saved creds (worker only)
    void worker_loop();           // drains the command queue, runs ops serially
    static void worker_trampoline(void* arg);
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The process-wide singleton (deliberately leaked, like app::agent_client()):
// destructors of other statics must not race a destroyed mutex at exit.
Manager& manager();

}  // namespace wifi
