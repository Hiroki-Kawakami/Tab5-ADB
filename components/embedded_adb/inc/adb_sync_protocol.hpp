// The `sync:` service sub-protocol — pure wire format, no I/O (like
// adb_protocol.hpp). After A_OPEN "sync:", every message on the stream is a
// little-endian 8-byte header (a 4-byte ASCII id + a 4-byte length/arg),
// optionally followed by a payload. These are the ids; the framing / state
// machine lives in the `adb` component's Sync session (it owns a thread).
//
// Reference: AOSP platform/system/core adb file_sync_protocol.h.
#pragma once

#include <cstdint>

namespace adb {
namespace sync {

// Header id values, read/written little-endian (so the on-wire bytes spell the
// ASCII tag: 'S','T','A','T' for STAT, etc.). v1 protocol only — the v2 ids
// (STA2/LST2/DNT2/SND2/RCV2) add 64-bit fields we don't need yet.
constexpr uint32_t ID_STAT = 0x54415453;  // 'STAT' — lstat a path
constexpr uint32_t ID_LIST = 0x5453494C;  // 'LIST' — list a directory
constexpr uint32_t ID_DENT = 0x544E4544;  // 'DENT' — one directory entry
constexpr uint32_t ID_SEND = 0x444E4553;  // 'SEND' — push a file
constexpr uint32_t ID_RECV = 0x56434552;  // 'RECV' — pull a file
constexpr uint32_t ID_DATA = 0x41544144;  // 'DATA' — a data chunk
constexpr uint32_t ID_DONE = 0x454E4F44;  // 'DONE' — end of data / listing
constexpr uint32_t ID_OKAY = 0x59414B4F;  // 'OKAY' — success status
constexpr uint32_t ID_FAIL = 0x4C494146;  // 'FAIL' — failure status + message
constexpr uint32_t ID_QUIT = 0x54495551;  // 'QUIT' — end the sync session

// Max payload of one sync DATA chunk. The actual A_WRTE is further capped to the
// connection's negotiated max_payload (DATA header included), since
// AdbConnection::send_write() does not split.
constexpr uint32_t DATA_MAX = 64 * 1024;

// Max path length adbd accepts in a request (file_sync_protocol.h SyncRequest).
constexpr uint32_t PATH_MAX = 1024;

}  // namespace sync
}  // namespace adb
