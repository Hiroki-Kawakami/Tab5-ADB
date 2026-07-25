// Six-digit Wireless debugging pairing client. Android's pairing service is a
// separate TLS endpoint from ADB-over-TCP and authorizes the persisted RSA key
// that the later ADB STARTTLS connection presents.
#include "adb_pairing.hpp"

#include "adb_pairing_crypto.hpp"
#include "adb_pairing_protocol.hpp"
#include "adb_spake2.hpp"
#include "adb_tcp_socket.hpp"
#include "adb_tls_stream.hpp"

#include <mbedtls/platform_util.h>
#include <psa/crypto.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <vector>

namespace adb {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kConnectTimeoutMs = 5000;
constexpr uint32_t kIoWakeTimeoutMs = 1000;
constexpr size_t kGcmTagSize = 16;
constexpr uint8_t kRsaPublicKeyType = 0;
constexpr uint8_t kDeviceGuidType = 1;

// Keep one deadline across every phase: the on-device pairing service and code
// are short-lived, so no successful phase should reset the caller's timeout.
class Deadline {
public:
    explicit Deadline(uint32_t timeout_ms)
        : end_(Clock::now() + std::chrono::milliseconds(timeout_ms)) {}

    uint32_t remaining_ms() const {
        auto now = Clock::now();
        if (now >= end_) {
            return 0;
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_ - now).count();
        return static_cast<uint32_t>(std::max<int64_t>(remaining, 1));
    }

private:
    Clock::time_point end_;
};

class SocketGuard {
public:
    explicit SocketGuard(int fd) : fd_(fd) {}
    ~SocketGuard() { internal::close_tcp_socket(fd_); }
    int get() const { return fd_; }

private:
    int fd_;
};

bool valid_code(std::string_view code) {
    if (code.size() != 6) {
        return false;
    }
    return std::all_of(code.begin(), code.end(), [](char c) {
        return c >= '0' && c <= '9';
    });
}

PairingError write_packet(internal::TlsClientStream& tls,
                          pairing::PairingPacketType type,
                          const uint8_t* payload, size_t payload_size,
                          const Deadline& deadline) {
    if (!payload || payload_size == 0 ||
        payload_size > pairing::kMaxPairingPayloadSize) {
        return PairingError::Protocol;
    }
    auto header = pairing::encode_pairing_header(
        type, static_cast<uint32_t>(payload_size));
    uint32_t remaining = deadline.remaining_ms();
    if (remaining == 0) {
        return PairingError::Timeout;
    }
    if (!tls.write_all(header.data(), header.size(), remaining)) {
        return deadline.remaining_ms() == 0
                   ? PairingError::Timeout
                   : PairingError::Protocol;
    }
    remaining = deadline.remaining_ms();
    if (remaining == 0) {
        return PairingError::Timeout;
    }
    if (!tls.write_all(payload, payload_size, remaining)) {
        return deadline.remaining_ms() == 0
                   ? PairingError::Timeout
                   : PairingError::Protocol;
    }
    return PairingError::Ok;
}

PairingError read_header(internal::TlsClientStream& tls,
                         pairing::PairingPacketType expected_type,
                         pairing::PairingPacketHeader& header,
                         const Deadline& deadline) {
    std::array<uint8_t, 6> encoded{};
    uint32_t remaining = deadline.remaining_ms();
    if (remaining == 0) {
        return PairingError::Timeout;
    }
    internal::TlsIoResult result = tls.read_exact(
        encoded.data(), encoded.size(), false, remaining);
    if (result == internal::TlsIoResult::Timeout) {
        return PairingError::Timeout;
    }
    if (result != internal::TlsIoResult::Ok ||
        !pairing::decode_pairing_header(encoded.data(), header) ||
        header.type != expected_type) {
        return PairingError::Protocol;
    }
    return PairingError::Ok;
}

PairingError read_payload(internal::TlsClientStream& tls,
                          uint8_t* payload, size_t payload_size,
                          const Deadline& deadline) {
    uint32_t remaining = deadline.remaining_ms();
    if (remaining == 0) {
        return PairingError::Timeout;
    }
    internal::TlsIoResult result = tls.read_exact(
        payload, payload_size, false, remaining);
    if (result == internal::TlsIoResult::Timeout) {
        return PairingError::Timeout;
    }
    return result == internal::TlsIoResult::Ok
               ? PairingError::Ok
               : PairingError::Protocol;
}

PairingResult failure(PairingError error) {
    return {error, {}};
}

}  // namespace

const char* to_string(PairingError error) {
    switch (error) {
        case PairingError::Ok: return "Ok";
        case PairingError::InvalidTarget: return "InvalidTarget";
        case PairingError::InvalidCode: return "InvalidCode";
        case PairingError::Connect: return "Connect";
        case PairingError::Timeout: return "Timeout";
        case PairingError::Tls: return "Tls";
        case PairingError::KeyExport: return "KeyExport";
        case PairingError::Crypto: return "Crypto";
        case PairingError::Protocol: return "Protocol";
        case PairingError::Authentication: return "Authentication";
    }
    return "?";
}

PairingResult pair_tcp(const std::string& host, uint16_t port,
                       std::string_view code, const RsaKey& key,
                       uint32_t timeout_ms) {
    if (host.empty() || port == 0 || timeout_ms == 0) {
        return failure(PairingError::InvalidTarget);
    }
    if (!valid_code(code)) {
        return failure(PairingError::InvalidCode);
    }

    Deadline deadline(timeout_ms);
    uint32_t connect_timeout = std::min(timeout_ms, kConnectTimeoutMs);
    uint32_t io_timeout = std::min(timeout_ms, kIoWakeTimeoutMs);
    int fd = internal::connect_tcp_socket(
        host, port, connect_timeout, io_timeout);
    if (fd < 0) {
        return failure(deadline.remaining_ms() == 0
                           ? PairingError::Timeout
                           : PairingError::Connect);
    }
    SocketGuard socket(fd);

    internal::TlsClientStream tls;
    uint32_t remaining = deadline.remaining_ms();
    if (remaining == 0) {
        return failure(PairingError::Timeout);
    }
    if (!tls.handshake(socket.get(), key, remaining)) {
        return failure(deadline.remaining_ms() == 0
                           ? PairingError::Timeout
                           : PairingError::Tls);
    }

    // ADB includes the C-string terminator in this exporter label.
    static constexpr char kExporterLabel[] = "adb-label";
    std::array<uint8_t, 64> exporter{};
    if (!tls.export_keying_material(
            kExporterLabel, sizeof(kExporterLabel),
            exporter.data(), exporter.size())) {
        return failure(PairingError::KeyExport);
    }

    // Binding the code to this TLS session prevents replay on another channel.
    std::vector<uint8_t> password(code.begin(), code.end());
    password.insert(password.end(), exporter.begin(), exporter.end());
    mbedtls_platform_zeroize(exporter.data(), exporter.size());

    std::array<uint8_t, 64> random{};
    if (psa_generate_random(random.data(), random.size()) != PSA_SUCCESS) {
        mbedtls_platform_zeroize(password.data(), password.size());
        return failure(PairingError::Crypto);
    }
    pairing::Spake2 spake(pairing::Spake2Role::Alice);
    bool initialized = spake.init(
        password.data(), password.size(), random.data());
    mbedtls_platform_zeroize(random.data(), random.size());
    mbedtls_platform_zeroize(password.data(), password.size());
    if (!initialized) {
        return failure(PairingError::Crypto);
    }

    PairingError error = write_packet(
        tls, pairing::PairingPacketType::Spake2Message,
        spake.message().data(), spake.message().size(), deadline);
    if (error != PairingError::Ok) {
        return failure(error);
    }

    pairing::PairingPacketHeader header{};
    error = read_header(tls, pairing::PairingPacketType::Spake2Message,
                        header, deadline);
    if (error != PairingError::Ok) {
        return failure(error);
    }
    if (header.payload_size != 32) {
        return failure(PairingError::Protocol);
    }
    std::array<uint8_t, 32> peer_message{};
    error = read_payload(tls, peer_message.data(), peer_message.size(),
                         deadline);
    if (error != PairingError::Ok) {
        return failure(error);
    }

    std::array<uint8_t, 64> key_material{};
    if (!spake.process_message(peer_message.data(), peer_message.size(),
                               key_material)) {
        return failure(PairingError::Authentication);
    }
    pairing::PairingCipher cipher;
    initialized = cipher.init(key_material.data(), key_material.size());
    mbedtls_platform_zeroize(key_material.data(), key_material.size());
    if (!initialized) {
        return failure(PairingError::Crypto);
    }

    constexpr size_t kPeerInfoSize = pairing::kMaxPeerInfoSize;
    constexpr size_t kEncryptedPeerInfoSize =
        kPeerInfoSize + kGcmTagSize;
    // PeerInfo is fixed-size and zero-padded on the wire. One heap allocation
    // keeps both 8 KiB records off the embedded task stack; the zero fill also
    // terminates the public-key string.
    std::vector<uint8_t> workspace(
        kPeerInfoSize + kEncryptedPeerInfoSize, 0);
    uint8_t* peer_info = workspace.data();
    uint8_t* encrypted = workspace.data() + kPeerInfoSize;

    std::string public_key;
    if (!key.android_public_key(public_key) ||
        public_key.size() > kPeerInfoSize - 2) {
        return failure(PairingError::Crypto);
    }
    peer_info[0] = kRsaPublicKeyType;
    std::memcpy(peer_info + 1, public_key.data(), public_key.size());

    size_t encrypted_len = 0;
    if (!cipher.encrypt(peer_info, kPeerInfoSize, encrypted,
                        kEncryptedPeerInfoSize, encrypted_len) ||
        encrypted_len != kEncryptedPeerInfoSize) {
        return failure(PairingError::Crypto);
    }
    error = write_packet(tls, pairing::PairingPacketType::PeerInfo,
                         encrypted, encrypted_len, deadline);
    if (error != PairingError::Ok) {
        return failure(error);
    }

    error = read_header(tls, pairing::PairingPacketType::PeerInfo,
                        header, deadline);
    if (error != PairingError::Ok) {
        return failure(error);
    }
    if (header.payload_size != kEncryptedPeerInfoSize) {
        return failure(PairingError::Protocol);
    }
    error = read_payload(tls, encrypted, header.payload_size, deadline);
    if (error != PairingError::Ok) {
        return failure(error);
    }

    size_t peer_info_len = 0;
    if (!cipher.decrypt(encrypted, header.payload_size, peer_info,
                        kPeerInfoSize, peer_info_len)) {
        return failure(PairingError::Authentication);
    }
    if (peer_info_len != kPeerInfoSize ||
        peer_info[0] != kDeviceGuidType) {
        return failure(PairingError::Protocol);
    }

    const uint8_t* guid = peer_info + 1;
    size_t guid_capacity = kPeerInfoSize - 1;
    const void* terminator = std::memchr(guid, 0, guid_capacity);
    if (!terminator || terminator == guid) {
        return failure(PairingError::Protocol);
    }
    size_t guid_len = static_cast<const uint8_t*>(terminator) - guid;
    return {
        PairingError::Ok,
        std::string(reinterpret_cast<const char*>(guid), guid_len),
    };
}

}  // namespace adb
