// Simulator Wi-Fi backend — a deterministic, scriptable fake.
//
// No real host-network access (that would make simverify non-deterministic and
// hand the agent the host's Wi-Fi). Instead it returns a canned AP list and
// scripted connect outcomes, delivered on a short-lived worker thread to mimic
// the device's async event delivery (so the UI's Connecting state is observable).
// Control via wifi::sim::* (wifi_sim.hpp), driven by the sim harness / env vars.
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "esp_log.h"
#include "wifi_backend.hpp"
#include "wifi_sim.hpp"

namespace wifi {

static const char* TAG = "wifi.sim";

namespace {

struct Fake {
    std::mutex mtx;
    std::vector<AP> aps = {
        {"OfficeWiFi", -45, true},
        {"Tab5-Guest", -60, true},
        {"OpenCafe",   -72, false},
    };
    bool has_forced = false;
    Result forced = Result::Ok;
    int delay_ms = 300;
    std::string ip = "192.168.1.50";
    int8_t cur_rssi = 0;
    BackendHost* host = nullptr;
    bool connected = false;
};

Fake& fake() {
    static Fake* f = new Fake();
    return *f;
}

// Run `fn` after the configured delay on a detached thread (mimics async events).
template <typename Fn>
void async_after(int delay_ms, Fn fn) {
    std::thread([delay_ms, fn = std::move(fn)]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        fn();
    }).detach();
}

Result heuristic_result(const std::string& ssid) {
    auto has = [&](const char* s) { return ssid.find(s) != std::string::npos; };
    if (has("fail-auth")) return Result::AuthFailed;
    if (has("fail-notfound")) return Result::ApNotFound;
    if (has("fail-assoc")) return Result::AssocFailed;
    return Result::Ok;
}

class SimBackend : public Backend {
public:
    explicit SimBackend(BackendHost* host) { fake().host = host; }

    void start() override {
        // Optional env overrides for non-scripted headless runs.
        if (const char* v = std::getenv("SIMULATOR_WIFI_CONNECT")) {
            std::string s(v);
            Result r = Result::Ok;
            if (s == "ApNotFound") r = Result::ApNotFound;
            else if (s == "AuthFailed") r = Result::AuthFailed;
            else if (s == "AssocFailed") r = Result::AssocFailed;
            else if (s == "Timeout") r = Result::Timeout;  // handled as no-event below
            sim::set_next_connect_result(r);
        }
        ESP_LOGI(TAG, "fake wifi backend up");
    }

    void radio_on() override {}  // no real radio; the Manager owns the state
    void set_power_save(PowerSave) override {}  // no real modem to put to sleep
    void radio_off() override {
        std::lock_guard<std::mutex> lk(fake().mtx);
        fake().connected = false;
        fake().cur_rssi = 0;
    }

    void scan() override {
        std::vector<AP> aps;
        int delay;
        { auto& f = fake(); std::lock_guard<std::mutex> lk(f.mtx); aps = f.aps; delay = f.delay_ms; }
        async_after(delay, [aps = std::move(aps)]() mutable {
            fake().host->on_scan_result(Result::Ok, std::move(aps));
        });
    }

    void connect(const std::string& ssid, const std::string& /*password*/) override {
        Result r;
        int delay;
        int8_t rssi = -50;
        std::string ip;
        {
            auto& f = fake();
            std::lock_guard<std::mutex> lk(f.mtx);
            if (f.has_forced) { r = f.forced; f.has_forced = false; }
            else r = heuristic_result(ssid);
            // "timeout" SSID (or forced Timeout): deliver no event, let the
            // Manager's connect timeout fire.
            if (r == Result::Timeout || ssid.find("timeout") != std::string::npos) {
                ESP_LOGI(TAG, "connect '%s' -> (silent, will time out)", ssid.c_str());
                return;
            }
            delay = f.delay_ms;
            ip = f.ip;
            for (auto& ap : f.aps) if (ap.ssid == ssid) rssi = ap.rssi;
            f.cur_rssi = (r == Result::Ok) ? rssi : 0;
        }
        async_after(delay, [r, ip]() {
            auto* host = fake().host;
            if (r == Result::Ok) {
                host->on_associated();
                { std::lock_guard<std::mutex> lk(fake().mtx); fake().connected = true; }
                host->on_got_ip(ip);
            } else {
                host->on_disconnected(r);
            }
        });
    }

    void disconnect() override {
        std::lock_guard<std::mutex> lk(fake().mtx);
        fake().connected = false;
        fake().cur_rssi = 0;
    }

    int8_t rssi() override {
        std::lock_guard<std::mutex> lk(fake().mtx);
        return fake().cur_rssi;
    }

    std::string mac() override { return "02:00:5e:c0:ff:ee"; }  // fixed fake STA MAC
};

}  // namespace

namespace sim {

void set_aps(std::vector<AP> aps) {
    auto& f = fake();
    std::lock_guard<std::mutex> lk(f.mtx);
    f.aps = std::move(aps);
}
void set_next_connect_result(Result r) {
    auto& f = fake();
    std::lock_guard<std::mutex> lk(f.mtx);
    f.has_forced = true;
    f.forced = r;
}
void set_event_delay_ms(int ms) {
    auto& f = fake();
    std::lock_guard<std::mutex> lk(f.mtx);
    f.delay_ms = ms;
}
void set_ip(std::string ip) {
    auto& f = fake();
    std::lock_guard<std::mutex> lk(f.mtx);
    f.ip = std::move(ip);
}
void drop_link() {
    auto* host = fake().host;
    { std::lock_guard<std::mutex> lk(fake().mtx); fake().connected = false; fake().cur_rssi = 0; }
    if (host) host->on_disconnected(Result::Failed);
}

}  // namespace sim

Backend* make_backend(BackendHost* host) { return new SimBackend(host); }

}  // namespace wifi
