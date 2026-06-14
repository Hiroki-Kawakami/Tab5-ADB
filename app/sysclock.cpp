#include "sysclock.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/time.h>

namespace app::sysclock {

bool parse_date_z(const std::string& out, time_t* epoch, int* tz_offset_min) {
    // Expect "<epoch> ±HHMM" — a single line, possibly trailing whitespace.
    char* end = nullptr;
    long long secs = std::strtoll(out.c_str(), &end, 10);
    if (end == out.c_str() || secs <= 0) return false;

    // Skip to the offset token.
    while (*end == ' ' || *end == '\t') ++end;
    char sign = *end;
    if (sign != '+' && sign != '-') return false;
    ++end;
    // Need 4 digits HHMM.
    for (int i = 0; i < 4; ++i)
        if (end[i] < '0' || end[i] > '9') return false;
    int hh = (end[0] - '0') * 10 + (end[1] - '0');
    int mm = (end[2] - '0') * 10 + (end[3] - '0');
    if (mm >= 60) return false;
    int off = hh * 60 + mm;
    if (sign == '-') off = -off;

    *epoch = (time_t)secs;
    *tz_offset_min = off;
    return true;
}

std::string posix_tz_from_offset(int tz_offset_min) {
    // POSIX TZ offset = value added to local to reach UTC = -(east offset).
    int p = -tz_offset_min;
    char sign = p < 0 ? '-' : '+';
    if (p < 0) p = -p;
    int hh = p / 60, mm = p % 60;
    char buf[24];
    if (mm)
        std::snprintf(buf, sizeof(buf), "UTC%c%d:%02d", sign, hh, mm);
    else
        std::snprintf(buf, sizeof(buf), "UTC%c%d", sign, hh);
    return buf;
}

std::string format_stamp(time_t t) {
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[24];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tmv);
    return buf;
}

void apply(time_t epoch, int tz_offset_min) {
    struct timeval tv = {};
    tv.tv_sec = epoch;
    settimeofday(&tv, nullptr);
    setenv("TZ", posix_tz_from_offset(tz_offset_min).c_str(), 1);
    tzset();
}

bool is_set() {
    // 2024-01-01 UTC — anything past it means the clock was set this boot (the
    // unset clock starts at the epoch / a small boot uptime).
    return time(nullptr) > 1704067200;
}

std::string dated_path(const char* dir, const char* prefix, const char* ext) {
    char path[96];
    struct stat st;
    if (is_set()) {
        std::string stamp = format_stamp(time(nullptr));
        std::snprintf(path, sizeof(path), "%s/%s_%s.%s", dir, prefix,
                      stamp.c_str(), ext);
        if (stat(path, &st) != 0) return path;
        // Same-second collision (rare): disambiguate with a counter.
        for (int i = 1; i < 1000; ++i) {
            std::snprintf(path, sizeof(path), "%s/%s_%s_%d.%s", dir, prefix,
                          stamp.c_str(), i, ext);
            if (stat(path, &st) != 0) return path;
        }
        return "";
    }
    // No RTC / not synced yet: first free <prefix>_NNN.<ext>.
    for (int i = 0; i < 1000; ++i) {
        std::snprintf(path, sizeof(path), "%s/%s_%03d.%s", dir, prefix, i, ext);
        if (stat(path, &st) != 0) return path;
    }
    return "";
}

}  // namespace app::sysclock
