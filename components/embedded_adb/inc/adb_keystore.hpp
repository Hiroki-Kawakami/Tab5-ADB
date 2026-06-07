// Persistence for the host's ADB RSA identity. The key lives in NVS (the
// ESP-IDF nvs C API on both targets — flash on device, JSON in the simulator),
// per the project's NVS rule.
//
// We deliberately DO NOT read the host PC's ~/.android/adbkey; the Tab5 has its
// own identity, generated once and stored in NVS. The device will prompt the
// user to authorize this identity on first connect.
#pragma once

#include <optional>

#include "adb_crypto.hpp"

namespace adb {

// Load the persisted RSA key, generating and storing a new one on first run.
// Returns nullopt only on unrecoverable NVS/crypto failure.
std::optional<RsaKey> load_or_create_key(const char* nvs_namespace = "adb");

}  // namespace adb
