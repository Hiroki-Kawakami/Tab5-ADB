// TLS 1.3 client shared by ADB STARTTLS and six-digit pairing. It owns the
// mbedTLS state but borrows the socket, whose lifetime remains with the caller.
#pragma once

#include <cstddef>
#include <cstdint>

#include "adb_crypto.hpp"

namespace adb {
namespace internal {

enum class TlsIoResult {
    Ok,
    Timeout,
    Error,
};

class TlsClientStream {
public:
    TlsClientStream();
    ~TlsClientStream();

    TlsClientStream(const TlsClientStream&) = delete;
    TlsClientStream& operator=(const TlsClientStream&) = delete;

    bool handshake(int fd, const RsaKey& key, uint32_t timeout_ms);
    bool write_all(const uint8_t* data, size_t len,
                   uint32_t timeout_ms = 0);
    // At a record boundary, idle_timeout turns the first socket wake into
    // Timeout so the long-lived ADB read loop can observe stop requests.
    TlsIoResult read_exact(uint8_t* data, size_t len,
                           bool idle_timeout,
                           uint32_t timeout_ms = 0);
    bool export_keying_material(const char* label, size_t label_len,
                                uint8_t* output, size_t output_len);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace internal
}  // namespace adb
