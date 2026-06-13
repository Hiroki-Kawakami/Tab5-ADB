#pragma once
#include <cstddef>
#include <string>
#include <vector>

// Recent ADB-over-TCP targets ("host:port"), most-recent-first. Persisted in NVS
// (namespace "tab5adb", key "tcp_history") as a newline-separated string — so a
// dev tool can pre-seed it by editing the simulator's JSON-backed nvs_data.json
// (gitignored), which the simverify TCP flow then reads back. The Wireless card on
// HomeScreen renders the list; tapping an entry reconnects to that target (wired
// once the TCP transport lands).
namespace app {
namespace tcp_history {

// Max entries kept; the oldest are dropped past this. Change here to tune the cap.
// Sized so the Recent list fits on HomeScreen without scrolling.
inline constexpr std::size_t kCap = 5;

// Saved targets, most-recent-first (empty if none / the key is unset).
std::vector<std::string> load();

// Record a target: move it to the front, dedupe (exact match), cap to kCap, and
// persist. Empty / whitespace-only targets are ignored.
void add(const std::string& target);

// Drop a target if present and persist; no-op when absent. Backs a delete UI.
void remove(const std::string& target);

}  // namespace tcp_history
}  // namespace app
