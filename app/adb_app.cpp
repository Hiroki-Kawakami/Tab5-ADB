#include "adb_app.hpp"

#include <memory>

#include "adb.hpp"  // adb::Client, adb::ClientListener
#include "bsp.h"
#include "display_manager.hpp"
#include "lvgl.hpp"
#include "screen_manager.hpp"
#include "home_screen.hpp"

namespace app {
namespace {

// Single-device holder: the Client must outlive the screens (they are pushed/
// popped, the connection is not), so it lives at file scope. The holder is the
// app's ClientListener — on_state fires on the reader thread, so everything
// UI-facing is marshalled to LVGL with lv_async_call. The Client holds the
// listener weakly, so the holder is owned by a shared_ptr it hands in.
std::shared_ptr<adb::Client> g_client;

class Holder : public adb::ClientListener {
public:
    void start(std::weak_ptr<adb::ClientListener> self,
               std::function<void(bool)> on_result) {
        on_result_ = std::move(on_result);
        reported_ = false;
        g_client = adb::Client::connect_usb(std::move(self));
    }

    void on_state(adb::Client* /*c*/, adb::ConnectionState s) override {
        if (s == adb::ConnectionState::Online) {
            report(true);
        } else if (s == adb::ConnectionState::Closed) {
            report(false);  // closed before/without ever reaching Online
        }
    }

private:
    void report(bool ok) {
        if (reported_) return;  // deliver the result once (Online then Closed, etc.)
        reported_ = true;
        auto cb = on_result_;
        lv_async_call([cb, ok]() { if (cb) cb(ok); });
    }

    std::function<void(bool)> on_result_;
    bool reported_ = false;
};

std::shared_ptr<Holder> g_holder = std::make_shared<Holder>();

}  // namespace

void adb_connect_async(std::function<void(bool)> on_result) {
    g_holder->start(g_holder, std::move(on_result));
}

adb::Client* adb_client() { return g_client.get(); }

std::shared_ptr<adb::Client> adb_client_shared() { return g_client; }

}  // namespace app

void adb_app() {
    bsp_config_t config = {};
    config.display.fb_num = 2;
    config.display.pixel_format = BSP_PIXEL_FORMAT_RGB565;
    config.usb.usb5v_en = true;
    ESP_ERROR_CHECK(bsp_init(&config));

    display_manager.init();
    lv_async_call([](){
        screen_manager.push(std::make_shared<HomeScreen>());
    });
    bsp_display_set_brightness(80);
}
