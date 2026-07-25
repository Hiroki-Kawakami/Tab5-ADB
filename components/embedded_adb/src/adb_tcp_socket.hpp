// Shared BSD-socket/lwIP setup for normal ADB-over-TCP and the pairing endpoint.
// The I/O timeout is a wake interval for higher-level cancellation/deadlines,
// not the lifetime of the connection.
#pragma once

#include <cstdint>
#include <string>

namespace adb {
namespace internal {

int connect_tcp_socket(const std::string& host, uint16_t port,
                       uint32_t connect_timeout_ms,
                       uint32_t io_timeout_ms);
void close_tcp_socket(int& fd);

}  // namespace internal
}  // namespace adb
