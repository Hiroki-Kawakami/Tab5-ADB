// Internal backend seam for wifi::Manager — the one device/simulator split.
//
// The Manager (wifi_manager.cpp, portable) owns all policy: the singleton, the
// one-shot completion bookkeeping, the connect timeout, NVS persistence, Status
// caching, and Listener dispatch. A Backend only does the radio-level work and
// reports raw events back through BackendHost. make_backend() is defined once per
// target (backend_espwifi.cpp on device, backend_sim.cpp in the simulator),
// selected by the ESP_PLATFORM branch in CMakeLists.txt.
//
// All BackendHost callbacks may arrive on any backend thread; the Manager
// serializes them under its own mutex.
#pragma once

#include <string>
#include <vector>

#include "wifi_manager.hpp"

namespace wifi {

// Events a Backend reports up to the Manager.
class BackendHost {
public:
    virtual ~BackendHost() = default;
    virtual void on_scan_result(Result r, std::vector<AP> aps) = 0;
    virtual void on_associated() = 0;                  // AP joined, pre-IP
    virtual void on_got_ip(const std::string& ip) = 0; // usable
    // A disconnect/association-failure. `reason` is the backend's best mapping of
    // why (used to complete a pending connect; ignored for a steady-state drop).
    virtual void on_disconnected(Result reason) = 0;
};

class Backend {
public:
    virtual ~Backend() = default;

    // Bring up the stack + radio in STA mode (idempotent). The host is already
    // set before this is called.
    virtual void start() = 0;

    // Toggle the radio without tearing down the initialized stack. start() leaves
    // the radio on; these turn it off (esp_wifi_stop) and back on (esp_wifi_start).
    virtual void radio_on() = 0;
    virtual void radio_off() = 0;

    virtual void scan() = 0;
    virtual void connect(const std::string& ssid, const std::string& password) = 0;
    virtual void disconnect() = 0;

    // Current associated-AP RSSI (dBm), or 0 if unavailable.
    virtual int8_t rssi() = 0;
};

// Per-target factory.
Backend* make_backend(BackendHost* host);

}  // namespace wifi
