// ADB wire protocol — pure data, no I/O, no target-specific dependencies.
// Modeled on the original implementation (adb.h, types.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace adb {

// Wire commands (adb.h). Values are little-endian four-char codes.
enum Command : uint32_t {
    A_SYNC = 0x434e5953,
    A_CNXN = 0x4e584e43,  // connect: establish session
    A_OPEN = 0x4e45504f,  // open a stream to a service
    A_OKAY = 0x59414b4f,  // ready / flow-control ack
    A_CLSE = 0x45534c43,  // close a stream
    A_WRTE = 0x45545257,  // stream data
    A_AUTH = 0x48545541,  // authentication
    A_STLS = 0x534c5453,  // start TLS
};

// A_AUTH arg0 — authentication sub-type (adb.h).
enum AuthType : uint32_t {
    ADB_AUTH_TOKEN        = 1,  // device -> host: 20-byte challenge
    ADB_AUTH_SIGNATURE    = 2,  // host -> device: RSA signature of the token
    ADB_AUTH_RSAPUBLICKEY = 3,  // host -> device: our public key (first contact)
};

// Protocol version carried in CNXN.arg0 (adb.h).
constexpr uint32_t A_VERSION_MIN = 0x01000000;  // baseline
constexpr uint32_t A_VERSION     = 0x01000001;  // skip-checksum capable

// Version carried in A_STLS.arg0 (adb.h) — the ADB STARTTLS handshake used by
// Android 11+ wireless debugging.
constexpr uint32_t A_STLS_VERSION = 0x01000000;

constexpr size_t ADB_TOKEN_SIZE = 20;    // TOKEN_SIZE (adb.h)
constexpr size_t MAX_PAYLOAD_V1 = 4096;  // pre-negotiation cap (adb.h)

// 24-byte on-the-wire message header (amessage, types.h). Sent verbatim, so the
// layout must match exactly: 6 packed little-endian uint32_t.
#pragma pack(push, 1)
struct MessageHeader {
    uint32_t command;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t data_length;
    uint32_t data_check;
    uint32_t magic;  // command ^ 0xffffffff
};
#pragma pack(pop)
static_assert(sizeof(MessageHeader) == 24, "ADB header must be 24 bytes");

// Sum of payload bytes — the data_check value (adb.cpp calculate_apacket_checksum).
uint32_t payload_checksum(const uint8_t* data, size_t len);

// A full ADB packet: header + optional payload (apacket, types.h).
struct Packet {
    MessageHeader header{};
    std::vector<uint8_t> payload;

    // Build a packet and fill the derived header fields from the payload.
    static Packet make(uint32_t command, uint32_t arg0, uint32_t arg1,
                       std::vector<uint8_t> data = {});

    // Recompute data_length / data_check / magic from the current payload.
    void finalize();

    // Validate magic and (when present) the payload checksum of a received packet.
    bool valid() const;
};

}  // namespace adb
