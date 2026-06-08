// Pure wire format for the tab5adb-agent protocol — the contract in
// android-agent/docs/protocol.md, with NO I/O (the agent_link counterpart of
// embedded_adb's adb_protocol.hpp). Both the Tab5 side (this component) and the
// agent (Java) follow protocol.md; keep this header in sync with §3/§4 there.
//
// All multi-byte values are little-endian (ESP32-P4 and Android-ARM are both LE).
#pragma once

#include <cstddef>
#include <cstdint>

namespace agent_link {

// proto_version: exact match required (protocol.md §4.4).
constexpr uint8_t kProtoVersion = 1;

// Frame header (protocol.md §3): MAGIC + 7 bytes, payload follows.
constexpr uint8_t kMagic = 0xA5;
constexpr size_t kFrameHeaderSize = 8;

// TYPE (§3.1).
enum Type : uint8_t {
    kTypeControlRequest = 0x01,
    kTypeControlResponse = 0x02,
    kTypeEvent = 0x03,
    kTypeJpeg = 0x10,
    kTypeAudio = 0x11,
};

// FLAGS bits (§3.2).
enum Flag : uint8_t {
    kFlagFrameStart = 0x01,
    kFlagFrameEnd = 0x02,
};

// Control commands (§4.4).
enum Cmd : uint8_t {
    kCmdHello = 0x01,
};

// Status codes (§4.5).
enum Status : uint8_t {
    kStatusOk = 0x00,
    kStatusEinval = 0x01,
    kStatusEnotsup = 0x02,  // proto mismatch
    kStatusEbusy = 0x03,
    kStatusEstate = 0x04,
    kStatusErange = 0x05,
    kStatusEfail = 0xFF,
};

// video_codec (HELLO request, §4.4).
constexpr uint8_t kVideoCodecJpeg = 0x01;

// HELLO request args length (§4.4) and response result length (§4.4).
constexpr size_t kHelloArgsLen = 10;     // after cmd+req_id
constexpr size_t kHelloResultLen = 12;   // after cmd+req_id+status

// --- little-endian field access ---
inline uint16_t rd_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t rd_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline void wr_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void wr_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

// Decoded frame header (§3).
struct FrameHeader {
    uint8_t magic;
    uint8_t type;
    uint8_t flags;
    uint8_t seq;
    uint32_t length;
};

// Parse the 8-byte header from `p` (caller guarantees >= kFrameHeaderSize bytes).
// Returns false if MAGIC is wrong (a sync-loss / corruption sentinel, §8).
inline bool parse_header(const uint8_t* p, FrameHeader& h) {
    h.magic = p[0];
    h.type = p[1];
    h.flags = p[2];
    h.seq = p[3];
    h.length = rd_u32(p + 4);
    return h.magic == kMagic;
}

// Write an 8-byte header into `p` (caller guarantees >= kFrameHeaderSize bytes).
inline void write_header(uint8_t* p, uint8_t type, uint8_t flags, uint8_t seq,
                         uint32_t length) {
    p[0] = kMagic;
    p[1] = type;
    p[2] = flags;
    p[3] = seq;
    wr_u32(p + 4, length);
}

}  // namespace agent_link
