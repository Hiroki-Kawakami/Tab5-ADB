// ADB-over-TCP transport. ADB's wire protocol is identical over TCP — the same
// 24-byte header + payload, just streamed on a socket instead of framed by USB
// bulk transfers — so this is the ONE transport that is the SAME on both targets:
// lwip's BSD sockets on device, the host's on the simulator. No ESP_PLATFORM
// split, no idf_compat shim (sockets are a standard contract, not an Espressif
// API). The device must already be listening on TCP (classic `adb tcpip`/wireless
// debugging); reaching that mode is out of scope here.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <psa/crypto.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>  // MBEDTLS_ERR_NET_* codes for the BIO callbacks
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "adb_transport.hpp"

namespace adb {

namespace {

constexpr unsigned kConnectTimeoutMs = 5000;
constexpr unsigned kReadTimeoutMs = 1000;  // recv() wakes this often so the read
                                           // loop can see a stop() request

void log(const char* msg) { std::fprintf(stderr, "[adb/tcp] %s\n", msg); }

// BIO glue: mbedTLS reads/writes the raw socket through these. EAGAIN (the
// SO_RCVTIMEO wake) maps to WANT_READ/WRITE so the handshake/read loop retries.
int bio_send(void* ctx, const unsigned char* buf, size_t len) {
    int fd = *static_cast<int*>(ctx);
    ssize_t n = ::send(fd, buf, len, 0);
    if (n >= 0) return static_cast<int>(n);
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

int bio_recv(void* ctx, unsigned char* buf, size_t len) {
    int fd = *static_cast<int*>(ctx);
    ssize_t n = ::recv(fd, buf, len, 0);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return MBEDTLS_ERR_NET_CONN_RESET;  // peer closed
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

class TcpTransport : public Transport {
public:
    explicit TcpTransport(int fd) : fd_(fd) {}
    ~TcpTransport() override {
        close();
        if (tls_inited_) {
            mbedtls_ssl_free(&ssl_);
            mbedtls_ssl_config_free(&conf_);
            mbedtls_x509_crt_free(&cert_);
            mbedtls_pk_free(&pkey_);
        }
    }

    // ADB STARTTLS: present a self-signed cert from `key`, TLS 1.3 client
    // handshake over the socket. After this, send/recv route through mbedTLS.
    bool start_tls(const RsaKey& key) override {
        std::vector<uint8_t> key_der, cert_der;
        if (!key.to_der(key_der) || !key.self_signed_cert_der(cert_der)) {
            log("could not build TLS cert/key");
            return false;
        }

        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
        mbedtls_x509_crt_init(&cert_);
        mbedtls_pk_init(&pkey_);
        tls_inited_ = true;

        if (psa_crypto_init() != PSA_SUCCESS) {
            log("psa init failed");
            return false;
        }
        if (mbedtls_pk_parse_key(&pkey_, key_der.data(), key_der.size(), nullptr, 0) != 0) {
            log("pk parse failed");
            return false;
        }
        if (mbedtls_x509_crt_parse_der(&cert_, cert_der.data(), cert_der.size()) != 0) {
            log("cert parse failed");
            return false;
        }
        if (mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            log("ssl config failed");
            return false;
        }
        // adbd authenticates us by our client cert; we don't verify its cert (it is
        // self-signed and ephemeral). ADB STARTTLS is TLS 1.3.
        mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_min_tls_version(&conf_, MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_max_tls_version(&conf_, MBEDTLS_SSL_VERSION_TLS1_3);
        if (mbedtls_ssl_conf_own_cert(&conf_, &cert_, &pkey_) != 0) {
            log("own cert failed");
            return false;
        }
        if (mbedtls_ssl_setup(&ssl_, &conf_) != 0) {
            log("ssl setup failed");
            return false;
        }
        mbedtls_ssl_set_bio(&ssl_, &fd_, bio_send, bio_recv, nullptr);

        int rc;
        while ((rc = mbedtls_ssl_handshake(&ssl_)) != 0) {
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;  // SO_RCVTIMEO wake during the handshake — retry
            char buf[96];
            mbedtls_strerror(rc, buf, sizeof(buf));
            std::fprintf(stderr, "[adb/tcp] TLS handshake failed: -0x%04x %s\n",
                         -rc, buf);
            return false;
        }
        tls_active_ = true;
        return true;
    }

    bool write_packet(const Packet& p) override {
        if (!send_all(reinterpret_cast<const uint8_t*>(&p.header), sizeof(p.header))) {
            return false;
        }
        if (!p.payload.empty()) {
            return send_all(p.payload.data(), p.payload.size());
        }
        return true;
    }

    IoResult read_packet(Packet& p) override {
        // Header first. At a packet boundary a timeout just means an idle link.
        IoResult r = recv_all(reinterpret_cast<uint8_t*>(&p.header), sizeof(p.header),
                              /*at_boundary=*/true);
        if (r != IoResult::Ok) return r;

        uint32_t len = p.header.data_length;
        p.payload.resize(len);
        if (len > 0) {
            // The header promised this payload; a mid-packet stall is an error, not
            // an idle link.
            r = recv_all(p.payload.data(), len, /*at_boundary=*/false);
            if (r != IoResult::Ok) return IoResult::Error;
        }
        return IoResult::Ok;
    }

    void close() override {
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    bool send_all(const uint8_t* data, size_t len) {
        if (tls_active_) return tls_send_all(data, len);
        size_t off = 0;
        while (off < len) {
            ssize_t n = ::send(fd_, data + off, len - off, 0);
            if (n > 0) {
                off += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            log("send failed");
            return false;
        }
        return true;
    }

    // Read exactly len bytes. recv() carries SO_RCVTIMEO, so a timeout surfaces as
    // EAGAIN/EWOULDBLOCK: at a packet boundary with nothing read yet that means an
    // idle link (Timeout, the read loop re-checks stop()); otherwise keep waiting
    // for the rest of the in-flight packet.
    IoResult recv_all(uint8_t* data, size_t len, bool at_boundary) {
        if (tls_active_) return tls_recv_all(data, len, at_boundary);
        size_t off = 0;
        while (off < len) {
            ssize_t n = ::recv(fd_, data + off, len - off, 0);
            if (n > 0) {
                off += static_cast<size_t>(n);
                continue;
            }
            if (n == 0) return IoResult::Error;  // peer closed the connection
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (off == 0 && at_boundary) return IoResult::Timeout;
                continue;  // mid-packet: the rest is coming
            }
            log(std::strerror(errno));
            return IoResult::Error;
        }
        return IoResult::Ok;
    }

    bool tls_send_all(const uint8_t* data, size_t len) {
        size_t off = 0;
        while (off < len) {
            int n = mbedtls_ssl_write(&ssl_, data + off, len - off);
            if (n > 0) {
                off += static_cast<size_t>(n);
                continue;
            }
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
            log("tls write failed");
            return false;
        }
        return true;
    }

    // Same boundary semantics as recv_all: WANT_READ at a boundary = idle Timeout.
    IoResult tls_recv_all(uint8_t* data, size_t len, bool at_boundary) {
        size_t off = 0;
        while (off < len) {
            int n = mbedtls_ssl_read(&ssl_, data + off, len - off);
            if (n > 0) {
                off += static_cast<size_t>(n);
                continue;
            }
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (off == 0 && at_boundary) return IoResult::Timeout;
                continue;  // mid-packet: the rest is coming
            }
            if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
            if (n == 0) {
                log("TLS peer closed connection");
            } else {
                char buf[96];
                mbedtls_strerror(n, buf, sizeof(buf));
                std::fprintf(stderr, "[adb/tcp] TLS read failed: %d %s\n", n, buf);
            }
            return IoResult::Error;
        }
        return IoResult::Ok;
    }

    int fd_;
    bool tls_inited_ = false;   // the mbedTLS contexts below were init'd
    bool tls_active_ = false;   // handshake done; route I/O through TLS
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config conf_;
    mbedtls_x509_crt cert_;
    mbedtls_pk_context pkey_;
};

// Non-blocking connect with a bounded timeout, so a wrong/unreachable target fails
// fast instead of hanging the reader task on a full TCP SYN backoff.
bool connect_with_timeout(int fd, const sockaddr* addr, socklen_t addrlen) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = ::connect(fd, addr, addrlen);
    if (rc != 0 && errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        timeval tv{static_cast<time_t>(kConnectTimeoutMs / 1000),
                   static_cast<suseconds_t>((kConnectTimeoutMs % 1000) * 1000)};
        rc = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
        if (rc <= 0) {
            log(rc == 0 ? "connect timed out" : "select failed");
            return false;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            log("connect failed");
            return false;
        }
    } else if (rc != 0) {
        log(std::strerror(errno));
        return false;
    }

    ::fcntl(fd, F_SETFL, flags);  // back to blocking
    return true;
}

}  // namespace

std::unique_ptr<Transport> open_tcp_transport(const std::string& host, uint16_t port) {
    char portstr[8];
    std::snprintf(portstr, sizeof(portstr), "%u", static_cast<unsigned>(port));

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;  // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), portstr, &hints, &res) != 0 || !res) {
        log("getaddrinfo failed");
        return nullptr;
    }

    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect_with_timeout(fd, ai->ai_addr, ai->ai_addrlen)) break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) return nullptr;

    // ADB does many small request/response round-trips; Nagle would add latency.
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // Bound recv() so the read loop periodically returns to check stop().
    timeval rcv{static_cast<time_t>(kReadTimeoutMs / 1000),
                static_cast<suseconds_t>((kReadTimeoutMs % 1000) * 1000)};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));

    return std::make_unique<TcpTransport>(fd);
}

}  // namespace adb
