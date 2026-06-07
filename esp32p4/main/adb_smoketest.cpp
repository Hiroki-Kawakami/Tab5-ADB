#include "adb_smoketest.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_log.h"
#include "embedded_adb.hpp"

namespace {

const char* TAG = "adb_smoke";

volatile bool s_online = false;

void reader_task(void* arg) {
    auto* conn = static_cast<adb::AdbConnection*>(arg);
    conn->run_blocking();  // pumps the read loop; handles CNXN/AUTH/streams
    vTaskDelete(nullptr);
}

void smoke_task(void* arg) {
    ESP_LOGI(TAG, "ADB smoke test: loading key...");
    auto key = adb::load_or_create_key();
    if (!key) {
        ESP_LOGE(TAG, "key load/create failed");
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "opening usb_host transport (plug in a phone, USB debugging on)...");
    auto transport = adb::open_usb_transport();
    if (!transport) {
        ESP_LOGE(TAG, "no ADB device on the USB host port");
        vTaskDelete(nullptr);
        return;
    }

    // Heap-allocated so it outlives this task's scope while the reader runs.
    // 16 KB advertised maxdata keeps usb_host DMA payload allocations modest.
    auto* conn = new adb::AdbConnection(std::move(transport), std::move(*key), 16 * 1024);
    conn->set_state_callback([](adb::ConnectionState s) {
        ESP_LOGI(TAG, "state -> %s", adb::to_string(s));
        if (s == adb::ConnectionState::Unauthorized) {
            ESP_LOGW(TAG, "tap \"Allow USB debugging?\" on the phone");
        }
        if (s == adb::ConnectionState::Online) s_online = true;
    });

    xTaskCreate(reader_task, "adb_reader", 8192, conn, 5, nullptr);

    for (int i = 0; i < 600 && !s_online; ++i) vTaskDelay(pdMS_TO_TICKS(100));
    if (!s_online) {
        ESP_LOGE(TAG, "did not reach Online");
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "ONLINE: %s", conn->banner().c_str());

    const char* services[] = {
        "shell:getprop ro.product.model",
        "shell:echo hello from tab5",
        "shell:id",
    };
    for (const char* svc : services) {
        std::string out;
        bool ok = conn->run_service(svc, out, 8000);
        ESP_LOGI(TAG, "[%s] ok=%d (%u bytes):\n%s", svc, ok,
                 static_cast<unsigned>(out.size()), out.c_str());
    }

    ESP_LOGI(TAG, "smoke test done");
    vTaskDelete(nullptr);
}

}  // namespace

void adb_smoketest_start() {
    // Big stack: first boot generates the RSA-2048 key (mbedTLS, stack-heavy).
    xTaskCreate(smoke_task, "adb_smoke", 16384, nullptr, 5, nullptr);
}
