#include "adb_app.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>

#include "adb.hpp"  // adb::Client, adb::ClientListener
#include "adb_transport.hpp"  // adb::set_usb_host_power_hook
#include "agent_client.hpp"
#include "bsp.h"
#include "display_manager.hpp"
#include "lvgl.hpp"
#include "modal.hpp"
#include "screen_manager.hpp"
#include "home_screen.hpp"
#include "settings.hpp"
#include "wifi_manager.hpp"

namespace app {
namespace {

// Single-device holder: the Client must outlive the screens (they are pushed/
// popped, the connection is not), so it lives at file scope. The holder is the
// app's ClientListener — on_state fires on the reader thread, so everything
// UI-facing is marshalled to LVGL with lv_async_call. The Client holds the
// listener weakly, so the holder is owned by a shared_ptr it hands in.
std::shared_ptr<adb::Client> g_client;

// Tracks whether ADB is currently connected, so apply_usb_host_power() can decide
// the VBUS state. Written from the reader thread (Holder::on_state); a plain bool
// is fine — apply_usb_host_power() only flips an I2C load switch.
bool g_adb_online = false;

// Set by adb_disconnect() (the Disconnect button) so the reader-thread Closed
// handler can tell a user-initiated disconnect from an unexpected one (cable
// pulled, device rebooted). Only the unexpected case shows the "Disconnected"
// notice and unwinds to the home screen; the button already navigates itself.
bool g_user_disconnect = false;

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
            g_adb_online = true;
            apply_usb_host_power();  // keep VBUS on for the live link
            report(true);
        } else if (s == adb::ConnectionState::Closed) {
            bool was_online = g_adb_online;
            g_adb_online = false;
            apply_usb_host_power();  // cut VBUS when the Connected policy is set
            report(false);  // closed before/without ever reaching Online
            // The adb link is gone, so the tab5adb-agent connection is too: tear it
            // down (a later feature use re-launches it). on_state is on the reader
            // thread; AgentClient marshals its own cleanup.
            agent_client().on_adb_disconnected();
            // An unexpected mid-session drop (cable pulled, device reset): unwind to
            // the home screen and tell the user. A connect-time failure (never
            // reached Online) is reported via on_result instead, and a user-driven
            // Disconnect navigates itself — skip both. Marshal to LVGL.
            if (was_online && !g_user_disconnect) {
                lv_async_call([]() {
                    screen_manager.load(std::make_shared<HomeScreen>());
                    app::modal_message(lv_screen_active(), "Disconnected",
                                       "The device was disconnected.");
                });
            }
            g_user_disconnect = false;
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

void apply_usb_host_power() {
    bool on = usb_host_power() == UsbHostPower::Always || g_adb_online;
    bsp_usb_host_set_power(on);
}

adb::Client* adb_client() { return g_client.get(); }

std::shared_ptr<adb::Client> adb_client_shared() { return g_client; }

void adb_disconnect() {
    if (!g_client) return;
    // Mark this as a user-initiated disconnect so the Closed handler doesn't show
    // the "Disconnected" notice / navigate home — the caller does that itself.
    g_user_disconnect = true;
    // close() blocks until the reader task exits; on the way out it fires
    // on_state(Closed) on the reader thread, which tears down the agent link
    // (Holder::on_state -> agent_client().on_adb_disconnected()).
    g_client->close();
    g_client.reset();
}

}  // namespace app

void adb_app() {
    bsp_config_t config = {};
    // Three framebuffers: normal LVGL uses the first two as a double buffer; the
    // mirror rotates all three as a triple buffer so its decode task never has to
    // wait on the panel scan-out/vsync sync before reusing a buffer.
    config.display.fb_num = 3;
    // Panel pixel format is fixed for the boot (changing it needs a restart). The
    // Settings screen persists the user's choice (default RGB888 — avoids the 565
    // banding on the mirror's gradients); the DisplayManager, the overlay compositor
    // and the mirror decode all honour bsp_display_get_pixel_format().
    config.display.pixel_format =
        (app::display_color_depth() == app::ColorDepth::Color16)
            ? BSP_PIXEL_FORMAT_RGB565
            : BSP_PIXEL_FORMAT_RGB888;
    // Boot VBUS follows the persisted USB host power policy: Always powers the host
    // port at boot; Connected leaves it off (we boot disconnected) and only powers
    // it for a live link. Connecting re-powers the port via the transport reset hook
    // regardless, so a device plugged before Connect still enumerates on the rising
    // VBUS edge.
    config.usb.usb5v_en = (app::usb_host_power() == app::UsbHostPower::Always);
    // Enable the touch controller INT so the DisplayManager touch task can block on
    // it (interrupt-driven wake) and idle when untouched instead of polling forever.
    config.touch.interrupt = true;
    // Speaker route policy from the persisted setting (default Auto: speaker on
    // only while no headphone is plugged, so plugging in headphones for the mirror
    // audio silences the speaker automatically).
    config.audio.speaker_mode = (app::speaker_mode() == app::SpeakerMode::Off)
                                    ? BSP_AUDIO_SPEAKER_MODE_OFF
                                    : BSP_AUDIO_SPEAKER_MODE_AUTO;
    ESP_ERROR_CHECK(bsp_init(&config));

    // Teach the USB transport how to reset the host port (called once after the
    // host stack is up, so a device plugged in before Connect re-attaches with a
    // clean connect edge). embedded_adb stays board-agnostic — it knows only
    // "reset"; the VBUS power-cycle (USB5V_EN off→settle→on) lives here. No-op on
    // the simulator (the libusb transport never calls it).
    adb::set_usb_host_reset_hook([] {
        bsp_usb_host_set_power(false);
        vTaskDelay(pdMS_TO_TICKS(200));  // let the phone observe VBUS removal
        bsp_usb_host_set_power(true);
    });

    // Apply the persisted audio settings (the volume is the gain the next stream
    // fades in to; the EQ override sticks across HP-route re-voicing).
    bsp_audio_set_volume(app::master_volume());
    bsp_audio_set_eq_enabled(app::equalizer_enabled());

    display_manager.init();
    lv_async_call([](){
        screen_manager.push(std::make_shared<HomeScreen>());
    });
    bsp_display_set_brightness(app::display_brightness());

    // Bring Wi-Fi up and auto-connect to the saved network. Non-blocking: the wifi
    // manager runs the blocking esp_wifi bring-up on its own worker task, so boot
    // never stalls. Status flows to the HomeScreen via the wifi::Listener it
    // registers.
    wifi::manager().autoconnect_saved();
}
