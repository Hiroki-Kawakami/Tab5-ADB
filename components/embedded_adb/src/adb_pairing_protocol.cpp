#include "adb_pairing_protocol.hpp"

namespace adb {
namespace pairing {

namespace {

constexpr uint8_t kPairingVersion = 1;

}  // namespace

std::array<uint8_t, 6> encode_pairing_header(
    PairingPacketType type, uint32_t payload_size) {
    return {
        kPairingVersion,
        static_cast<uint8_t>(type),
        static_cast<uint8_t>(payload_size >> 24),
        static_cast<uint8_t>(payload_size >> 16),
        static_cast<uint8_t>(payload_size >> 8),
        static_cast<uint8_t>(payload_size),
    };
}

bool decode_pairing_header(const uint8_t data[6],
                           PairingPacketHeader& header) {
    if (!data || data[0] != kPairingVersion ||
        data[1] > static_cast<uint8_t>(PairingPacketType::PeerInfo)) {
        return false;
    }
    uint32_t payload_size =
        (static_cast<uint32_t>(data[2]) << 24) |
        (static_cast<uint32_t>(data[3]) << 16) |
        (static_cast<uint32_t>(data[4]) << 8) |
        static_cast<uint32_t>(data[5]);
    if (payload_size == 0 || payload_size > kMaxPairingPayloadSize) {
        return false;
    }
    header.type = static_cast<PairingPacketType>(data[1]);
    header.payload_size = payload_size;
    return true;
}

}  // namespace pairing
}  // namespace adb
