// Simulator-only control surface for the fake Wi-Fi backend.
//
// The simulator backend is a deterministic fake (no real host-network access) so
// simverify can drive the Wi-Fi UI reproducibly. The sim harness (and tests) call
// these to script scan results, connect outcomes, latency, and mid-session link
// drops. Has NO effect / no definition on device — include only from sim code.
#pragma once

#include <string>
#include <vector>

#include "wifi_manager.hpp"

namespace wifi {
namespace sim {

// Replace the canned scan AP list returned by the next scan().
void set_aps(std::vector<AP> aps);

// Force the outcome of the NEXT connect() (consumed once). Without this, the fake
// decides from the SSID: contains "fail-auth"->AuthFailed, "fail-notfound"->
// ApNotFound, "fail-assoc"->AssocFailed, "timeout"->(no event, lets the Manager
// time out), else Ok.
void set_next_connect_result(Result r);

// Latency (ms) the fake waits before delivering scan/connect events on a worker
// thread (mimics the device's async event delivery). Default 300.
void set_event_delay_ms(int ms);

// The IP the fake reports on a successful connect (default "192.168.1.50").
void set_ip(std::string ip);

// Inject a mid-session link drop (fires on_disconnected, as if the AP vanished),
// to exercise the persistent Listener path.
void drop_link();

void register_harness_commands();

}  // namespace sim
}  // namespace wifi
