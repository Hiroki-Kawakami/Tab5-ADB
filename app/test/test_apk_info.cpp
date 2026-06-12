// Host unit test for app/apk_info — parses the checked-in fixtures under
// simulator/sdcard/ (testapp.apk is a real, installable APK; dummy.apk is a
// plain text file that must fail gracefully). No phone, no LVGL.
//
// Run via the app test runner:
//   TEST=test_apk_info nix develop -c app/test/run.sh
// argv[1] = repo root (the runner passes it).

#include <cassert>
#include <cstdio>
#include <string>

#include "apk_info.hpp"

using namespace app::apkinfo;

static int g_checks = 0;
#define CHECK(cond)                                                  \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                \
        }                                                            \
    } while (0)

int main(int argc, char** argv) {
    std::string repo = argc > 1 ? argv[1] : ".";

    // The real test APK (com.tab5adb.testapp, UTF-16 string pool, literal label).
    {
        ApkInfo info;
        std::string err;
        bool ok = parse((repo + "/simulator/sdcard/testapp.apk").c_str(), info, err);
        if (!ok) std::fprintf(stderr, "parse error: %s\n", err.c_str());
        CHECK(ok);
        CHECK(info.package == "com.tab5adb.testapp");
        CHECK(info.label == "Tab5 Test App");
        CHECK(info.version_name == "1.0");
        CHECK(info.version_code == 1);
        CHECK(info.min_sdk > 0);
        CHECK(info.target_sdk >= info.min_sdk);
        std::printf("testapp.apk: pkg=%s label=\"%s\" ver=%s (%u) sdk=%d..%d\n",
                    info.package.c_str(), info.label.c_str(),
                    info.version_name.c_str(), info.version_code, info.min_sdk,
                    info.target_sdk);
    }

    // Not a zip at all -> clean failure, no field leaks.
    {
        ApkInfo info;
        std::string err;
        CHECK(!parse((repo + "/simulator/sdcard/dummy.apk").c_str(), info, err));
        CHECK(!err.empty());
        CHECK(info.package.empty());
        std::printf("dummy.apk: rejected (%s)\n", err.c_str());
    }

    // Missing file -> clean failure.
    {
        ApkInfo info;
        std::string err;
        CHECK(!parse((repo + "/simulator/sdcard/no_such.apk").c_str(), info, err));
    }

    std::printf("PASSED (%d checks)\n", g_checks);
    return 0;
}
