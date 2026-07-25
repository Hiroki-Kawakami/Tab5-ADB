// Outer framing for Android's pairing protocol: version byte, packet-type byte,
// and a big-endian 32-bit payload length followed by the payload.
#pragma once

#include <array>
#include <cstdint>

namespace adb {
namespace pairing {

// PeerInfo is a fixed 8 KiB plaintext record. The framing decoder uses the
// protocol's broader ceiling; each exchange phase still enforces its exact size.
constexpr uint32_t kMaxPeerInfoSize = 8192;
constexpr uint32_t kMaxPairingPayloadSize = kMaxPeerInfoSize * 2;

enum class PairingPacketType : uint8_t {
    Spake2Message = 0,
    PeerInfo = 1,
};

struct PairingPacketHeader {
    PairingPacketType type;
    uint32_t payload_size;
};

std::array<uint8_t, 6> encode_pairing_header(PairingPacketType type,
                                              uint32_t payload_size);
bool decode_pairing_header(const uint8_t data[6],
                           PairingPacketHeader& header);

}  // namespace pairing
}  // namespace adb
