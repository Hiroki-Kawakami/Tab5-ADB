// Device Wi-Fi backend — esp_wifi / esp_netif / esp_event (STA).
//
// On the Tab5 (ESP32-P4) esp_wifi is esp_wifi_remote, routed over the SDIO
// esp-hosted link to the ESP32-C6 co-processor. The transport is brought up by
// esp_wifi_init() itself, driven by sdkconfig (SDIO pins / reset GPIO / slave
// target) — so this backend is plain esp_wifi with no board-specific bring-up,
// the standard ESP-IDF lifecycle the project keeps out of the BSP.
//
// Modeled on the reference NetworkManager but recast as a wifi::Backend: events
// are translated and pushed up through BackendHost; all policy lives in the
// portable Manager.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <algorithm>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_wifi_netif.h"
#include "esp_private/wifi.h"

#include "wifi_backend.hpp"

namespace wifi {

static const char* TAG = "wifi.esp";

namespace {

// The updated ESP32-C6 esp-hosted firmware fires the STA START/STOP events
// redundantly (or while the netif is already up), and IDF's default
// wifi_default_action_sta_start (= wifi_start) crashes on the second
// invocation (double esp_netif_action_start / rxcb re-register). So instead of
// esp_netif_create_default_wifi_sta() — whose default handlers are internal and
// can't be guarded — we create the STA netif manually and register our own
// idempotent start/stop handlers (the rest of the lifecycle uses IDF's public
// action functions). Mirrors M5Tab5-UserDemo's "fix wifi init crash with
// updated c6 firmware" (which patched the AP path; this is its STA analogue).

bool s_sta_netif_started = false;

void sta_start_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* netif = static_cast<esp_netif_t*>(arg);
    if (s_sta_netif_started || esp_netif_is_netif_up(netif)) {
        ESP_LOGW(TAG, "ignore duplicate Wi-Fi STA start event");
        return;
    }
    auto driver = static_cast<wifi_netif_driver_t>(esp_netif_get_io_driver(netif));
    uint8_t mac[6];
    esp_err_t ret = esp_wifi_get_if_mac(driver, mac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_if_mac failed: %s", esp_err_to_name(ret));
        return;
    }
    if (esp_wifi_is_if_ready_when_started(driver)) {
        ret = esp_wifi_register_if_rxcb(driver, esp_netif_receive, netif);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_register_if_rxcb failed: %s", esp_err_to_name(ret));
            return;
        }
    }
    ret = esp_wifi_internal_reg_netstack_buf_cb(esp_netif_netstack_buf_ref,
                                                esp_netif_netstack_buf_free);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "netstack cb register failed: %s", esp_err_to_name(ret));
        return;
    }
    esp_netif_set_mac(netif, mac);
    esp_netif_action_start(netif, base, id, data);
    s_sta_netif_started = true;
}

void sta_stop_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* netif = static_cast<esp_netif_t*>(arg);
    if (!s_sta_netif_started && !esp_netif_is_netif_up(netif)) {
        ESP_LOGW(TAG, "ignore duplicate Wi-Fi STA stop event");
        return;
    }
    esp_netif_action_stop(netif, base, id, data);
    s_sta_netif_started = false;
}

void sta_connected_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* netif = static_cast<esp_netif_t*>(arg);
    auto driver = static_cast<wifi_netif_driver_t>(esp_netif_get_io_driver(netif));
    if (!esp_wifi_is_if_ready_when_started(driver)) {
        // interface not ready at start → register the rxcb on connection
        esp_err_t ret = esp_wifi_register_if_rxcb(driver, esp_netif_receive, netif);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_register_if_rxcb failed: %s", esp_err_to_name(ret));
            return;
        }
    }
    esp_netif_action_connected(netif, base, id, data);
}

esp_netif_t* create_wifi_remote_sta_netif() {
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_STA();
    esp_netif_t* netif = esp_netif_new(&cfg);
    assert(netif);
    ESP_ERROR_CHECK(esp_netif_attach_wifi_station(netif));
    // The netif-plumbing handlers (separate from the app-level wifi_event/
    // ip_event handlers EspWifiBackend registers). disconnected/got_ip use IDF's
    // public action functions verbatim; start/stop/connected are guarded copies.
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, WIFI_EVENT_STA_START, sta_start_handler, netif));
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, WIFI_EVENT_STA_STOP, sta_stop_handler, netif));
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, sta_connected_handler, netif));
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, esp_netif_action_disconnected, netif));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, esp_netif_action_got_ip, netif));
    return netif;
}

Result reason_to_result(uint8_t reason, bool was_associated) {
    if (was_associated) return Result::IpFailed;  // joined then dropped pre-IP
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
            return Result::ApNotFound;
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return Result::AuthFailed;
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_CONNECTION_FAIL:
            return Result::AssocFailed;
        default:
            return Result::Failed;
    }
}

class EspWifiBackend : public Backend {
public:
    explicit EspWifiBackend(BackendHost* host) : host_(host) {}

    void start() override {
        if (started_) return;
        ESP_ERROR_CHECK(esp_netif_init());
        esp_err_t e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e);
        create_wifi_remote_sta_netif();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &EspWifiBackend::wifi_event, this, nullptr));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &EspWifiBackend::ip_event, this, nullptr));

        // The Manager owns credential persistence; keep the driver's own copy in
        // RAM so there is one source of truth.
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        started_ = true;
    }

    void radio_on() override {
        if (started_) esp_wifi_start();
    }
    void radio_off() override {
        if (started_) esp_wifi_stop();
    }

    void scan() override {
        wifi_scan_config_t sc = {};
        if (esp_wifi_scan_start(&sc, false) != ESP_OK)
            host_->on_scan_result(Result::Failed, {});
    }

    void connect(const std::string& ssid, const std::string& password) override {
        associated_ = false;
        wifi_config_t cfg = {};
        std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), ssid.c_str(),
                     sizeof(cfg.sta.ssid) - 1);
        std::strncpy(reinterpret_cast<char*>(cfg.sta.password), password.c_str(),
                     sizeof(cfg.sta.password) - 1);
        if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK ||
            esp_wifi_connect() != ESP_OK) {
            host_->on_disconnected(Result::Failed);
        }
    }

    void disconnect() override { esp_wifi_disconnect(); }

    void set_power_save(PowerSave mode) override {
        if (!started_) return;
        esp_wifi_set_ps(mode == PowerSave::None ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
    }

    int8_t rssi() override {
        wifi_ap_record_t info;
        return esp_wifi_sta_get_ap_info(&info) == ESP_OK ? info.rssi : 0;
    }

    std::string mac() override {
        uint8_t m[6];
        if (esp_wifi_get_mac(WIFI_IF_STA, m) != ESP_OK) return {};
        char buf[18];
        std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                      m[0], m[1], m[2], m[3], m[4], m[5]);
        return buf;
    }

private:
    static void wifi_event(void* arg, esp_event_base_t, int32_t id, void* data) {
        static_cast<EspWifiBackend*>(arg)->on_wifi(id, data);
    }
    static void ip_event(void* arg, esp_event_base_t, int32_t, void* data) {
        static_cast<EspWifiBackend*>(arg)->on_ip(data);
    }

    void on_wifi(int32_t id, void* data) {
        if (id == WIFI_EVENT_SCAN_DONE) {
            uint16_t n = 0;
            esp_wifi_scan_get_ap_num(&n);
            std::vector<wifi_ap_record_t> recs(n);
            if (n) esp_wifi_scan_get_ap_records(&n, recs.data());

            std::unordered_map<std::string, const wifi_ap_record_t*> best;
            for (const auto& r : recs) {
                if (r.ssid[0] == '\0') continue;
                std::string s(reinterpret_cast<const char*>(r.ssid));
                auto it = best.find(s);
                if (it == best.end() || r.rssi > it->second->rssi) best[s] = &r;
            }
            std::vector<AP> aps;
            aps.reserve(best.size());
            for (auto& [s, r] : best)
                aps.push_back({s, r->rssi, r->authmode != WIFI_AUTH_OPEN});
            std::sort(aps.begin(), aps.end(),
                      [](const AP& a, const AP& b) { return a.rssi > b.rssi; });
            host_->on_scan_result(Result::Ok, std::move(aps));
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            associated_ = true;
            host_->on_associated();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            auto* info = static_cast<wifi_event_sta_disconnected_t*>(data);
            bool was = associated_;
            associated_ = false;
            host_->on_disconnected(reason_to_result(info->reason, was));
        }
    }

    void on_ip(void* data) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        char ip[16];
        std::snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        associated_ = false;
        host_->on_got_ip(ip);
    }

    BackendHost* host_;
    bool started_ = false;
    volatile bool associated_ = false;  // joined AP, awaiting IP
};

}  // namespace

Backend* make_backend(BackendHost* host) { return new EspWifiBackend(host); }

}  // namespace wifi
