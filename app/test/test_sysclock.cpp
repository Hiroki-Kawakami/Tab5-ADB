// Host unit test for app/sysclock — the pure parsing/formatting functions
// (no I/O, no LVGL, no adb). No phone.

#include <cassert>
#include <cstdio>
#include <ctime>
#include <string>

#include "sysclock.hpp"

using namespace app::sysclock;

static int g_checks = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

int main() {
    // ---- parse_date_z -----------------------------------------------------
    time_t e = 0;
    int off = 0;

    CHECK(parse_date_z("1718337600 +0900", &e, &off));
    CHECK(e == 1718337600);
    CHECK(off == 540);

    CHECK(parse_date_z("1718337600 -0500", &e, &off));
    CHECK(off == -300);

    CHECK(parse_date_z("1718337600 +0530", &e, &off));
    CHECK(off == 330);

    CHECK(parse_date_z("1700000000 +0000", &e, &off));
    CHECK(off == 0);

    // Trailing newline (the shell appends one).
    CHECK(parse_date_z("1718337600 +0900\n", &e, &off));
    CHECK(off == 540);

    // Malformed → false, outputs untouched.
    e = 42, off = 99;
    CHECK(!parse_date_z("", &e, &off));
    CHECK(!parse_date_z("notanumber +0900", &e, &off));
    CHECK(!parse_date_z("1718337600", &e, &off));        // no offset
    CHECK(!parse_date_z("1718337600 0900", &e, &off));   // no sign
    CHECK(!parse_date_z("1718337600 +09", &e, &off));    // too short
    CHECK(!parse_date_z("1718337600 +0999", &e, &off));  // minutes >= 60
    CHECK(e == 42 && off == 99);

    // ---- posix_tz_from_offset (sign INVERTED vs ISO) ----------------------
    CHECK(posix_tz_from_offset(540) == "UTC-9");      // +0900 -> UTC-9
    CHECK(posix_tz_from_offset(-300) == "UTC+5");     // -0500 -> UTC+5
    CHECK(posix_tz_from_offset(330) == "UTC-5:30");   // +0530 -> UTC-5:30
    CHECK(posix_tz_from_offset(0) == "UTC+0");

    // The TZ string actually yields the intended local time via libc.
    setenv("TZ", posix_tz_from_offset(540).c_str(), 1);
    tzset();
    time_t t = 1718337600;  // 2024-06-14 04:00:00 UTC
    struct tm lt;
    localtime_r(&t, &lt);
    CHECK(lt.tm_hour == 13);  // +9h -> 13:00 JST
    CHECK(format_stamp(t) == "20240614_130000");

    std::printf("ok (%d checks)\n", g_checks);
    return 0;
}
