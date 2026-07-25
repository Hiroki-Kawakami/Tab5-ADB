// Android 11+ six-digit Wireless debugging pairing. This is the one-shot
// pairing-port protocol, not the later ADB connection on its separate port.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "adb_crypto.hpp"

namespace adb {

enum class PairingError {
    Ok,
    InvalidTarget,
    InvalidCode,
    Connect,
    Timeout,
    Tls,
    KeyExport,
    Crypto,
    Protocol,
    Authentication,
};

const char* to_string(PairingError error);

struct PairingResult {
    PairingError error = PairingError::Protocol;
    std::string device_guid;

    explicit operator bool() const { return error == PairingError::Ok; }
};

// Pair `key` with the device using exactly six ASCII digits. The call blocks
// through TCP, TLS, SPAKE2, and peer-info exchange; keep it off the UI thread.
// On success, reuse the same key for the normal ADB-over-TCP connection.
PairingResult pair_tcp(const std::string& host, uint16_t port,
                       std::string_view code, const RsaKey& key,
                       uint32_t timeout_ms = 15000);

}  // namespace adb
