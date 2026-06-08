#pragma once

#include <functional>
#include <memory>

// Panel geometry (M5Stack Tab5 / ILI9881C MIPI-DSI), native portrait.
constexpr int PANEL_W = 720;
constexpr int PANEL_H = 1280;

void adb_app();

namespace adb { class Client; }

// App-scoped ADB connection holder (implemented in adb_app.cpp). adb::Client owns
// the whole connection lifecycle + reader task; this is just the small app-level
// owner that keeps the single Client alive across the transient screens and
// marshals the Client's reader-thread state callbacks onto the LVGL thread (the
// library leaves UI marshalling to the app).
namespace app {

// Connect to the first USB device in the background. `on_result` runs on the LVGL
// thread: true once Online, false on failure. (Single-device: one connection.)
void adb_connect_async(std::function<void(bool ok)> on_result);

// The live Client, valid after a successful connect (else nullptr). Use it for
// everything connection-scoped, e.g. adb_client()->banner() / adb_client()->exec().
adb::Client* adb_client();

// The live Client as a shared_ptr (empty before connect). APIs that take
// ownership of the connection for their lifetime — e.g. agent_link::Link::open —
// need this rather than the raw pointer.
std::shared_ptr<adb::Client> adb_client_shared();

}  // namespace app
