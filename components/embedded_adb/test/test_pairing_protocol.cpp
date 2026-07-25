#include "adb_pairing_protocol.hpp"

#include <array>
#include <cstdio>

namespace {

int failures = 0;

#define CHECK(condition, message)                                    \
    do {                                                             \
        if (condition) {                                             \
            std::printf("  ok   %s\n", message);                    \
        } else {                                                     \
            std::printf("  FAIL %s\n", message);                    \
            ++failures;                                              \
        }                                                            \
    } while (0)

}  // namespace

int main() {
    std::printf("adb_pairing_protocol self-test\n");
    auto encoded = adb::pairing::encode_pairing_header(
        adb::pairing::PairingPacketType::PeerInfo, 8208);
    adb::pairing::PairingPacketHeader decoded{};
    CHECK(adb::pairing::decode_pairing_header(encoded.data(), decoded),
          "decode valid header");
    CHECK(decoded.type == adb::pairing::PairingPacketType::PeerInfo,
          "type round-trips");
    CHECK(decoded.payload_size == 8208, "big-endian size round-trips");

    auto invalid = encoded;
    invalid[0] = 2;
    CHECK(!adb::pairing::decode_pairing_header(invalid.data(), decoded),
          "reject unsupported version");

    invalid = encoded;
    invalid[1] = 2;
    CHECK(!adb::pairing::decode_pairing_header(invalid.data(), decoded),
          "reject unknown type");

    invalid = encoded;
    invalid[2] = 0;
    invalid[3] = 0;
    invalid[4] = 0;
    invalid[5] = 0;
    CHECK(!adb::pairing::decode_pairing_header(invalid.data(), decoded),
          "reject empty payload");

    invalid = adb::pairing::encode_pairing_header(
        adb::pairing::PairingPacketType::PeerInfo,
        adb::pairing::kMaxPairingPayloadSize + 1);
    CHECK(!adb::pairing::decode_pairing_header(invalid.data(), decoded),
          "reject oversized payload");

    std::printf("%s (%d failures)\n",
                failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
