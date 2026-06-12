#pragma once
#include <cstdint>
#include <string>

// Local APK metadata: zip central directory -> AndroidManifest.xml -> binary
// AXML. Pure parsing (POSIX file in, strings out) — no LVGL/adb deps, no
// device needed, so the APK preview works with adb disconnected and the parser
// is host-tested (app/test). Inflate via zlib (linked on both targets).
namespace app::apkinfo {

struct ApkInfo {
    std::string package;
    std::string version_name;
    std::string label;  // android:label only when it is a literal string —
                        // a resource reference (@0x7f...) stays empty
                        // (resources.arsc resolution is out of scope)
    uint32_t version_code = 0;
    int min_sdk = -1;   // -1 = not declared
    int target_sdk = -1;

    bool has(const std::string& s) const { return !s.empty(); }
};

// Parse the APK at `path`. Returns false (with `err` describing the stage)
// only when the file is not a readable APK — not a zip, no AndroidManifest.xml,
// or broken AXML. A field missing from the manifest is not an error; it keeps
// its default. (Parser contract: a field that fails to parse is hidden by the
// UI, never an error.)
bool parse(const char* path, ApkInfo& out, std::string& err);

}  // namespace app::apkinfo
