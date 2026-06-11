#include "device_info.hpp"

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace app::devinfo {

namespace {

// Split into lines (no terminators). Tolerates \r\n.
std::vector<std::string> lines_of(const std::string &s) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) {
            if (pos < s.size()) lines.push_back(s.substr(pos));
            break;
        }
        std::string line = s.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        pos = nl + 1;
    }
    return lines;
}

// Integer right after `needle` (first occurrence), or `fallback`.
long long int_after(const std::string &s, const char *needle, long long fallback) {
    size_t pos = s.find(needle);
    if (pos == std::string::npos) return fallback;
    pos += std::strlen(needle);
    while (pos < s.size() && s[pos] == ' ') ++pos;
    const char *p = s.c_str() + pos;
    char *end = nullptr;
    long long v = std::strtoll(p, &end, 10);
    return end == p ? fallback : v;
}

}  // namespace

// ---- Battery ----------------------------------------------------------------

const char *Battery::status_str() const {
    switch (status) {
        case 2: return "Charging";
        case 3: return "Discharging";
        case 4: return "Not charging";
        case 5: return "Full";
        default: return "Unknown";
    }
}

const char *Battery::health_str() const {
    switch (health) {
        case 2: return "Good";
        case 3: return "Overheat";
        case 4: return "Dead";
        case 5: return "Over voltage";
        case 6: return "Failure";
        case 7: return "Cold";
        default: return "Unknown";
    }
}

Battery parse_dumpsys_battery(const std::string &out) {
    Battery b;
    for (const auto &raw : lines_of(out)) {
        std::string line = trim(raw);
        if (line.rfind("AC powered:", 0) == 0 || line.rfind("USB powered:", 0) == 0 ||
            line.rfind("Wireless powered:", 0) == 0 || line.rfind("Dock powered:", 0) == 0) {
            if (line.find("true") != std::string::npos) b.powered = true;
        } else if (line.rfind("status:", 0) == 0) {
            b.status = (int)int_after(line, "status:", 0);
        } else if (line.rfind("health:", 0) == 0) {
            b.health = (int)int_after(line, "health:", 0);
        } else if (line.rfind("level:", 0) == 0) {
            b.level = (int)int_after(line, "level:", -1);
        } else if (line.rfind("voltage:", 0) == 0) {
            b.voltage_mV = (int)int_after(line, "voltage:", 0);
        } else if (line.rfind("temperature:", 0) == 0) {
            b.temp_dC = (int)int_after(line, "temperature:", 0);
        } else if (line.rfind("technology:", 0) == 0) {
            b.technology = trim(line.substr(std::strlen("technology:")));
        }
    }
    if (b.level < -1 || b.level > 100) b.level = -1;
    return b;
}

// ---- Wifi --------------------------------------------------------------------

int Wifi::bars() const {
    if (state != State::Connected) return 0;
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -80) return 2;
    return 1;
}

Wifi parse_wifi_status(const std::string &out) {
    Wifi w;
    if (out.find("Wifi is disabled") != std::string::npos) {
        w.state = Wifi::State::Off;
        return w;
    }
    if (out.find("Wifi is connected to") == std::string::npos) {
        // "Wifi is enabled" (or unparseable output) without a connection line.
        w.state = Wifi::State::Disconnected;
        return w;
    }
    w.state = Wifi::State::Connected;
    // First "RSSI:" is the active WifiInfo's (MLO link RSSIs come later).
    w.rssi = (int)int_after(out, "RSSI:", 0);
    size_t ip = out.find("IP: /");
    if (ip != std::string::npos) {
        ip += std::strlen("IP: /");
        size_t end = out.find_first_of(", \n", ip);
        w.ip = out.substr(ip, end == std::string::npos ? std::string::npos : end - ip);
    }
    return w;
}

// ---- Cellular -----------------------------------------------------------------

Cellular parse_cellular(const std::string &sim_state, const std::string &registry) {
    Cellular c;
    c.sim_present = sim_state.find("READY") != std::string::npos ||
                    sim_state.find("LOADED") != std::string::npos;
    c.in_service = registry.find("mVoiceRegState=0") != std::string::npos ||
                   registry.find("mDataRegState=0") != std::string::npos;
    // mSignalStrength holds one CellSignalStrength per RAT, each with a
    // "level=N" (the NR block writes "level = N"); the active RAT is whichever
    // reports the highest. Ignore values outside 0..4 (INT_MAX sentinels).
    size_t pos = 0;
    while ((pos = registry.find("level", pos)) != std::string::npos) {
        size_t p = pos + std::strlen("level");
        pos = p;
        while (p < registry.size() && registry[p] == ' ') ++p;
        if (p >= registry.size() || registry[p] != '=') continue;
        ++p;
        while (p < registry.size() && registry[p] == ' ') ++p;
        char *end = nullptr;
        long v = std::strtol(registry.c_str() + p, &end, 10);
        if (end != registry.c_str() + p && v >= 0 && v <= 4 && (int)v > c.level) c.level = (int)v;
    }
    return c;
}

// ---- Storage / Mem -------------------------------------------------------------

Storage parse_df(const std::string &out) {
    Storage s;
    for (const auto &line : lines_of(out)) {
        if (line.empty() || line.rfind("Filesystem", 0) == 0) continue;
        std::istringstream iss(line);
        std::string fs;
        int64_t total = 0, used = 0, avail = 0;
        if ((iss >> fs >> total >> used >> avail) && total > 0) {
            s.total_kb = total;
            s.used_kb = used;
            s.avail_kb = avail;
            break;
        }
    }
    return s;
}

Mem parse_meminfo(const std::string &out) {
    Mem m;
    m.total_kb = int_after(out, "MemTotal:", 0);
    m.avail_kb = int_after(out, "MemAvailable:", 0);
    return m;
}

// ---- CPU ------------------------------------------------------------------------

CpuTimes parse_proc_stat(const std::string &out) {
    CpuTimes t;
    for (const auto &line : lines_of(out)) {
        if (line.rfind("cpu", 0) != 0) continue;
        std::istringstream iss(line);
        std::string name;
        iss >> name;
        if (name != "cpu" && !(name.size() > 3 && std::isdigit((unsigned char)name[3]))) continue;
        uint64_t f[10] = {};
        int n = 0;
        while (n < 10 && (iss >> f[n])) ++n;
        if (n < 4) continue;
        CpuTimes::Core core;
        for (int i = 0; i < n; ++i) core.total += f[i];
        core.busy = core.total - f[3] - (n > 4 ? f[4] : 0);  // total - idle - iowait
        t.cores.push_back(core);
    }
    return t;
}

std::vector<int> cpu_usage(const CpuTimes &prev, const CpuTimes &now) {
    std::vector<int> usage;
    if (prev.cores.empty() || prev.cores.size() != now.cores.size()) return usage;
    usage.reserve(now.cores.size());
    for (size_t i = 0; i < now.cores.size(); ++i) {
        uint64_t dt = now.cores[i].total - prev.cores[i].total;
        uint64_t db = now.cores[i].busy - prev.cores[i].busy;
        int pct = dt > 0 ? (int)((db * 100 + dt / 2) / dt) : 0;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        usage.push_back(pct);
    }
    return usage;
}

// ---- getprop ----------------------------------------------------------------------

PropList parse_getprop(const std::string &out) {
    PropList props;
    for (const auto &line : lines_of(out)) {
        if (line.empty() || line[0] != '[') continue;
        size_t mid = line.find("]: [");
        if (mid == std::string::npos) continue;
        size_t end = line.rfind(']');
        if (end <= mid + 3) continue;
        props.emplace_back(line.substr(1, mid - 1), line.substr(mid + 4, end - mid - 4));
    }
    return props;
}

std::string prop(const PropList &props, const std::string &key) {
    for (const auto &kv : props)
        if (kv.first == key) return kv.second;
    return "";
}

// ---- formatters ---------------------------------------------------------------------

std::string trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string first_line(const std::string &s) {
    size_t nl = s.find('\n');
    return trim(nl == std::string::npos ? s : s.substr(0, nl));
}

std::string format_kb(int64_t kb) {
    char buf[32];
    if (kb >= 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f GB", (double)kb / (1024.0 * 1024.0));
    } else if (kb >= 1024) {
        std::snprintf(buf, sizeof(buf), "%" PRId64 " MB", (kb + 512) / 1024);
    } else {
        std::snprintf(buf, sizeof(buf), "%" PRId64 " kB", kb);
    }
    return buf;
}

int marketed_storage_gb(int64_t total_kb) {
    if (total_kb <= 0) return 0;
    double gib = (double)total_kb / (1024.0 * 1024.0);
    for (int s : {4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048})
        if (gib <= s) return s;
    return (int)gib;
}

int marketed_ram_gb(int64_t total_kb) {
    if (total_kb <= 0) return 0;
    double gib = (double)total_kb / (1024.0 * 1024.0);
    for (int s : {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64})
        if (gib <= s) return s;
    return (int)gib;
}

std::string format_temp_dC(int temp_dC) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d.%d°C", temp_dC / 10, std::abs(temp_dC % 10));
    return buf;
}

std::string format_uptime(const std::string &proc_uptime) {
    double secs = std::atof(proc_uptime.c_str());
    if (secs <= 0) return "";
    int64_t s = (int64_t)secs;
    int64_t d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
    char buf[32];
    if (d > 0) {
        std::snprintf(buf, sizeof(buf), "%" PRId64 "d %" PRId64 "h %" PRId64 "m", d, h, m);
    } else if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%" PRId64 "h %" PRId64 "m", h, m);
    } else {
        std::snprintf(buf, sizeof(buf), "%" PRId64 "m", m);
    }
    return buf;
}

std::string short_kernel(const std::string &proc_version) {
    // "Linux version 4.19.278-g7b09... (builder@host) (clang ...) #1 SMP ..."
    std::istringstream iss(first_line(proc_version));
    std::string a, b, c;
    if (iss >> a >> b >> c && a == "Linux" && b == "version") return c;
    return "";
}

std::vector<std::string> split_sections(const std::string &out, const std::string &sep) {
    std::vector<std::string> sections(1);
    for (const auto &line : lines_of(out)) {
        if (trim(line) == sep) {
            sections.emplace_back();
        } else {
            if (!sections.back().empty()) sections.back() += '\n';
            sections.back() += line;
        }
    }
    return sections;
}

}  // namespace app::devinfo
