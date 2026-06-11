#pragma once
// Parsers + formatters for device information fetched over adb shell — pure
// functions on strings (no I/O, no LVGL, no adb types) so they can run on the
// adb reader thread and be host-unit-tested (app/test/run.sh). Command output
// shapes differ across Android versions/vendors, so every parser treats
// malformed input as "absent": numeric fields keep their sentinel defaults and
// the caller hides that item instead of rendering garbage.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace app::devinfo {

// ---- parsed structs --------------------------------------------------------

// `dumpsys battery`
struct Battery {
    int level = -1;        // 0..100, -1 = parse failed
    bool powered = false;  // any of AC / USB / Wireless / Dock powered
    int status = 0;        // android.os.BatteryManager status (2=Charging, 5=Full)
    int health = 0;        // 2=Good, 3=Overheat, ...
    int voltage_mV = 0;
    int temp_dC = 0;       // tenths of a degree C (311 = 31.1 C)
    std::string technology;

    bool charging() const { return status == 2; }
    const char *status_str() const;
    const char *health_str() const;
};

// `cmd wifi status`
struct Wifi {
    enum class State { Off, Disconnected, Connected };
    State state = State::Off;
    int rssi = 0;    // dBm, valid when Connected
    std::string ip;  // from the WifiInfo line, may be empty

    int bars() const;  // 0..4 signal level (0 unless Connected)
};

// gsm.sim.state prop + a grep of `dumpsys telephony.registry`
struct Cellular {
    bool sim_present = false;
    bool in_service = false;
    int level = 0;  // 0..4 (max across the per-RAT CellSignalStrength levels)
};

// one row of `df -k <path>`
struct Storage {
    int64_t total_kb = 0, used_kb = 0, avail_kb = 0;
};

// /proc/meminfo
struct Mem {
    int64_t total_kb = 0, avail_kb = 0;
};

// one /proc/stat sample; cores[0] = the aggregate "cpu" line, [1..] = cpu0..N
struct CpuTimes {
    struct Core {
        uint64_t busy = 0, total = 0;
    };
    std::vector<Core> cores;
};

// ---- parsers ---------------------------------------------------------------

// Split a chained-exec output on lines equal to `sep` (the `echo ---SEP---`
// convention). Returns the sections with the separator lines removed.
std::vector<std::string> split_sections(const std::string &out,
                                        const std::string &sep = "---SEP---");

Battery parse_dumpsys_battery(const std::string &out);
Wifi parse_wifi_status(const std::string &out);
Cellular parse_cellular(const std::string &sim_state, const std::string &registry);
Storage parse_df(const std::string &out);
Mem parse_meminfo(const std::string &out);
CpuTimes parse_proc_stat(const std::string &out);

// Usage % per entry of `now` ([0] = total, [1..] = cores), from two consecutive
// /proc/stat samples. Empty when the samples don't line up.
std::vector<int> cpu_usage(const CpuTimes &prev, const CpuTimes &now);

// `getprop` full dump: "[key]: [value]" lines, file order preserved.
using PropList = std::vector<std::pair<std::string, std::string>>;
PropList parse_getprop(const std::string &out);
std::string prop(const PropList &props, const std::string &key);

// ---- formatters / small helpers --------------------------------------------

std::string trim(const std::string &s);        // strips spaces/CR/LF both ends
std::string first_line(const std::string &s);  // first line, trimmed

std::string format_kb(int64_t kb);  // "107.2 GB" / "512 MB"
// Round a df/meminfo total up to the marketed capacity ("128 GB" flash,
// "8 GB" RAM). 0 when the input is implausible (<= 0).
int marketed_storage_gb(int64_t total_kb);
int marketed_ram_gb(int64_t total_kb);

std::string format_temp_dC(int temp_dC);                    // 311 -> "31.1 C" (degree sign)
std::string format_uptime(const std::string &proc_uptime);  // /proc/uptime -> "1d 20h 40m"
std::string short_kernel(const std::string &proc_version);  // /proc/version -> "4.19.278-g..."

}  // namespace app::devinfo
