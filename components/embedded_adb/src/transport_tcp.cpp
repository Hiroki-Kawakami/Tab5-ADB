// ADB-over-TCP transport shared verbatim by the ESP32 lwIP and simulator BSD
// socket targets. It starts as the normal ADB byte stream and upgrades the same
// socket to TLS only when adbd sends A_STLS.
#include "adb_transport.hpp"

#include "adb_tcp_socket.hpp"
#include "adb_tls_stream.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>

namespace adb {

namespace {

constexpr uint32_t kConnectTimeoutMs = 5000;
constexpr uint32_t kIoTimeoutMs = 1000;
constexpr uint32_t kTlsHandshakeTimeoutMs = 15000;

void log(const char* message) {
    std::fprintf(stderr, "[adb/tcp] %s\n", message);
}

class TcpTransport : public Transport {
public:
    explicit TcpTransport(int fd) : fd_(fd) {}
    ~TcpTransport() override { close(); }

    bool start_tls(const RsaKey& key) override {
        auto tls = std::make_unique<internal::TlsClientStream>();
        if (!tls->handshake(fd_, key, kTlsHandshakeTimeoutMs)) {
            log("TLS handshake failed");
            return false;
        }
        tls_ = std::move(tls);
        return true;
    }

    bool write_packet(const Packet& packet) override {
        if (!send_all(reinterpret_cast<const uint8_t*>(&packet.header),
                      sizeof(packet.header))) {
            return false;
        }
        return packet.payload.empty() ||
               send_all(packet.payload.data(), packet.payload.size());
    }

    IoResult read_packet(Packet& packet) override {
        // A timeout before a header is an idle link. Once a header arrives, a
        // missing payload is a broken packet and therefore an error.
        IoResult result = receive_all(
            reinterpret_cast<uint8_t*>(&packet.header),
            sizeof(packet.header), true);
        if (result != IoResult::Ok) {
            return result;
        }

        uint32_t len = packet.header.data_length;
        packet.payload.resize(len);
        if (len == 0) {
            return IoResult::Ok;
        }
        result = receive_all(packet.payload.data(), len, false);
        return result == IoResult::Ok ? IoResult::Ok : IoResult::Error;
    }

    void close() override {
        tls_.reset();
        internal::close_tcp_socket(fd_);
    }

private:
    bool send_all(const uint8_t* data, size_t len) {
        if (tls_) {
            return tls_->write_all(data, len);
        }
        size_t offset = 0;
        while (offset < len) {
            ssize_t result = ::send(fd_, data + offset, len - offset, 0);
            if (result > 0) {
                offset += static_cast<size_t>(result);
                continue;
            }
            if (result < 0 &&
                (errno == EINTR || errno == EAGAIN ||
                 errno == EWOULDBLOCK)) {
                continue;
            }
            log("send failed");
            return false;
        }
        return true;
    }

    IoResult receive_all(uint8_t* data, size_t len, bool at_boundary) {
        if (tls_) {
            internal::TlsIoResult result =
                tls_->read_exact(data, len, at_boundary);
            if (result == internal::TlsIoResult::Ok) {
                return IoResult::Ok;
            }
            return result == internal::TlsIoResult::Timeout
                       ? IoResult::Timeout
                       : IoResult::Error;
        }

        size_t offset = 0;
        while (offset < len) {
            ssize_t result = ::recv(fd_, data + offset, len - offset, 0);
            if (result > 0) {
                offset += static_cast<size_t>(result);
                continue;
            }
            if (result == 0) {
                return IoResult::Error;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (offset == 0 && at_boundary) {
                    return IoResult::Timeout;
                }
                // The header already committed the peer to the remaining bytes.
                continue;
            }
            log(std::strerror(errno));
            return IoResult::Error;
        }
        return IoResult::Ok;
    }

    int fd_;
    std::unique_ptr<internal::TlsClientStream> tls_;
};

}  // namespace

std::unique_ptr<Transport> open_tcp_transport(const std::string& host,
                                               uint16_t port) {
    int fd = internal::connect_tcp_socket(
        host, port, kConnectTimeoutMs, kIoTimeoutMs);
    if (fd < 0) {
        log("connect failed");
        return nullptr;
    }
    return std::make_unique<TcpTransport>(fd);
}

}  // namespace adb
