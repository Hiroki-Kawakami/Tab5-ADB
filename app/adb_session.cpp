#include "adb_session.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "lvgl.hpp"  // lv_async_call (marshal to the LVGL thread)

namespace app {

namespace {

// usb_host DMA-allocs per payload on device, so keep the advertised CNXN maxdata
// modest there; the simulator (libusb) can afford the full ADB max.
#ifdef ESP_PLATFORM
constexpr uint32_t kMaxPayload = 16 * 1024;
#else
constexpr uint32_t kMaxPayload = 256 * 1024;
#endif

adb::AdbConnection* g_conn = nullptr;
std::string g_banner;

struct ConnectCtx {
    std::function<void(bool)> on_result;
};

void connect_worker(void* arg) {
    auto* ctx = static_cast<ConnectCtx*>(arg);

    // Deliver the result once, on the LVGL thread.
    bool reported = false;
    auto finish = [&](bool ok) {
        if (reported) return;
        reported = true;
        auto cb = ctx->on_result;
        lv_async_call([cb, ok]() { if (cb) cb(ok); });
    };

    auto key = adb::load_or_create_key();
    auto transport = key ? adb::open_usb_transport() : nullptr;
    if (!key || !transport) {
        finish(false);
        delete ctx;
        vTaskDelete(nullptr);
        return;
    }

    auto* conn = new adb::AdbConnection(std::move(transport), std::move(*key), kMaxPayload);
    conn->set_state_callback([&](adb::ConnectionState s) {
        if (s == adb::ConnectionState::Online) {
            g_conn = conn;
            g_banner = conn->banner();
            finish(true);
        } else if (s == adb::ConnectionState::Closed) {
            finish(false);  // closed before/without reaching Online
        }
    });

    conn->run_blocking();  // this task is the read loop; blocks until closed
    finish(false);         // run_blocking returned without ever going Online
    delete ctx;
    vTaskDelete(nullptr);
}

}  // namespace

void adb_connect_async(std::function<void(bool)> on_result) {
    auto* ctx = new ConnectCtx{std::move(on_result)};
    // Big stack: first run generates the RSA-2048 key (mbedTLS is stack-heavy).
    xTaskCreate(connect_worker, "adb_connect", 16384, ctx, 5, nullptr);
}

adb::AdbConnection* adb_connection() { return g_conn; }

const std::string& adb_banner() { return g_banner; }

}  // namespace app
