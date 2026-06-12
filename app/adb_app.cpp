#include "adb_app.hpp"

#include <memory>

#include "adb.hpp"  // adb::Client, adb::ClientListener
#include "agent_client.hpp"
#include "bsp.h"
#include "display_manager.hpp"
#include "lvgl.hpp"
#include "screen_manager.hpp"
#include "home_screen.hpp"
#include "settings.hpp"

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
            // The adb link is gone, so the tab5adb-agent connection is too: tear it
            // down (a later feature use re-launches it). on_state is on the reader
            // thread; AgentClient marshals its own cleanup.
            agent_client().on_adb_disconnected();
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
    config.usb.usb5v_en = true;
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

    // Apply the persisted audio settings (the volume is the gain the next stream
    // fades in to; the EQ override sticks across HP-route re-voicing).
    bsp_audio_set_volume(app::master_volume());
    bsp_audio_set_eq_enabled(app::equalizer_enabled());

    display_manager.init();
    lv_async_call([](){
        screen_manager.push(std::make_shared<HomeScreen>());
    });
    bsp_display_set_brightness(app::display_brightness());
}
