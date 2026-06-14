#pragma once
// System wall-clock set from the connected phone. The Tab5 has no battery-backed
// RTC, so the clock starts unset every boot; on adb Online we fetch the phone's
// time once (`date +'%s %z'`) and apply it here. Used to give captures/logs
// real dated filenames instead of an RTC-less sequence number.
//
// The parsing/formatting functions are pure (no I/O, no LVGL, no adb types) so
// they run on the adb reader thread and are host-unit-tested (app/test/run.sh).

#include <ctime>
#include <string>

namespace app::sysclock {

// Parse `date +'%s %z'` output, e.g. "1718337600 +0900". On success fills
// `epoch` (UTC seconds) + `tz_offset_min` (minutes EAST of UTC, +540 for
// +0900) and returns true; returns false (leaving the outputs untouched) when
// either field is malformed — the caller then just skips the sync.
bool parse_date_z(const std::string& out, time_t* epoch, int* tz_offset_min);

// POSIX TZ string for a fixed offset with no DST rules, e.g. +540 -> "UTC-9",
// -300 -> "UTC+5", +330 -> "UTC-5:30". The POSIX sign is INVERTED relative to
// ISO 8601 (the offset is the value added to local time to reach UTC).
std::string posix_tz_from_offset(int tz_offset_min);

// "20260614_153012" — local broken-down `t` formatted for a filename.
std::string format_stamp(time_t t);

// Set the system clock (settimeofday, UTC) and the process TZ (setenv + tzset)
// from values parsed out of `date +'%s %z'`. Callable from any thread.
void apply(time_t epoch, int tz_offset_min);

// True once the clock has been set to a plausible wall-clock time (i.e. apply()
// ran successfully this boot).
bool is_set();

// Build a filesystem path for a capture/log under `dir`: when the clock is set,
// "<dir>/<prefix>_YYYYMMDD_HHMMSS.<ext>" (a numeric suffix is appended on the
// rare same-second collision); otherwise the RTC-less fallback
// "<dir>/<prefix>_NNN.<ext>" (first free, NNN = 000..999). Returns "" when no
// free slot is found.
std::string dated_path(const char* dir, const char* prefix, const char* ext);

}  // namespace app::sysclock
