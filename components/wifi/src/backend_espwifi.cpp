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
#include <cstring>
#include <unordered_map>
#include <algorithm>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "wifi_backend.hpp"

namespace wifi {

static const char* TAG = "wifi.esp";

namespace {

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
        esp_netif_create_default_wifi_sta();

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

    int8_t rssi() override {
        wifi_ap_record_t info;
        return esp_wifi_sta_get_ap_info(&info) == ESP_OK ? info.rssi : 0;
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
