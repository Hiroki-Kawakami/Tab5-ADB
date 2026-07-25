#include "adb_tcp_socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

namespace adb {
namespace internal {

namespace {

// Use non-blocking mode only while connecting so an unreachable Wi-Fi target
// cannot consume the platform's full TCP SYN retry interval.
bool connect_with_timeout(int fd, const sockaddr* address,
                          socklen_t address_len,
                          uint32_t timeout_ms) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return false;
    }

    int result = ::connect(fd, address, address_len);
    if (result != 0 && errno == EINPROGRESS) {
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(fd, &write_fds);
        timeval timeout{
            static_cast<time_t>(timeout_ms / 1000),
            static_cast<suseconds_t>((timeout_ms % 1000) * 1000),
        };
        result = ::select(fd + 1, nullptr, &write_fds, nullptr, &timeout);
        if (result <= 0) {
            return false;
        }
        int error = 0;
        socklen_t error_len = sizeof(error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) != 0 ||
            error != 0) {
            return false;
        }
    } else if (result != 0) {
        return false;
    }

    // Packet I/O remains blocking; SO_RCVTIMEO/SO_SNDTIMEO provide wakeups.
    return ::fcntl(fd, F_SETFL, flags) == 0;
}

}  // namespace

int connect_tcp_socket(const std::string& host, uint16_t port,
                       uint32_t connect_timeout_ms,
                       uint32_t io_timeout_ms) {
    char port_string[8];
    std::snprintf(port_string, sizeof(port_string), "%u",
                  static_cast<unsigned>(port));

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    if (::getaddrinfo(host.c_str(), port_string, &hints, &addresses) != 0 ||
        !addresses) {
        return -1;
    }

    int fd = -1;
    for (addrinfo* address = addresses; address; address = address->ai_next) {
        fd = ::socket(address->ai_family, address->ai_socktype,
                      address->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect_with_timeout(fd, address->ai_addr,
                                 address->ai_addrlen,
                                 connect_timeout_ms)) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(addresses);
    if (fd < 0) {
        return -1;
    }

    // ADB consists of small request/response turns; Nagle adds visible latency.
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // Bounded socket waits let TLS and packet loops re-check their own deadline
    // or stop request.
    timeval timeout{
        static_cast<time_t>(io_timeout_ms / 1000),
        static_cast<suseconds_t>((io_timeout_ms % 1000) * 1000),
    };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return fd;
}

void close_tcp_socket(int& fd) {
    if (fd < 0) {
        return;
    }
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    fd = -1;
}

}  // namespace internal
}  // namespace adb
