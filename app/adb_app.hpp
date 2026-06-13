#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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

// The transport carrying the live (or most recent) ADB connection. USB by default
// before any connect. A general-purpose seam for features that must branch on the
// link kind — e.g. the mirror uses lighter JPEG params over the higher-latency,
// bandwidth-limited TCP/Wi-Fi link than over USB.
enum class Transport { Usb, Tcp };

// The transport of the live (or most recent) connection. Callable from any thread
// (a plain read). Returns Transport::Usb before the first connect.
Transport connection_transport();

// Convenience: true when the live (or most recent) connection is over ADB-over-TCP.
inline bool connection_is_tcp() { return connection_transport() == Transport::Tcp; }

// Connect to the first USB device in the background. `on_result` runs on the LVGL
// thread: true once Online, false on failure. (Single-device: one connection.)
void adb_connect_async(std::function<void(bool ok)> on_result);

// Connect over ADB-over-TCP to host:port (the device must be listening — `adb
// tcpip` / wireless debugging). Same single-device holder and `on_result`
// contract as adb_connect_async; the USB VBUS policy is left untouched for a TCP
// link.
void adb_connect_tcp_async(const std::string& host, uint16_t port,
                           std::function<void(bool ok)> on_result);

// The live Client, valid after a successful connect (else nullptr). Use it for
// everything connection-scoped, e.g. adb_client()->banner() / adb_client()->exec().
adb::Client* adb_client();

// The live Client as a shared_ptr (empty before connect). APIs that take
// ownership of the connection for their lifetime — e.g. agent_link::Link::open —
// need this rather than the raw pointer.
std::shared_ptr<adb::Client> adb_client_shared();

// Re-apply the USB host VBUS power policy (settings.hpp UsbHostPower) for the
// current connection state: VBUS stays on when the policy is Always or ADB is
// connected, and is cut when the policy is Connected and ADB is disconnected.
// Called on connect/disconnect and when the Settings toggle changes; the boot
// VBUS state is set directly from the policy in adb_app(). No-op effect on the
// simulator (bsp_usb_host_set_power is a no-op there).
void apply_usb_host_power();

// Tear down the active connection: close the Client (which stops the reader task
// and, via on_state(Closed), the tab5adb-agent link too) and release the holder's
// shared_ptr, so adb_client() is null again. Idempotent; call on the LVGL thread.
// The UI navigation back to HomeScreen is the caller's job.
void adb_disconnect();

}  // namespace app
