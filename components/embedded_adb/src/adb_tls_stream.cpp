#include "adb_tls_stream.hpp"

#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <vector>

namespace adb {
namespace internal {

namespace {

using Clock = std::chrono::steady_clock;

bool timed_out(Clock::time_point start, uint32_t timeout_ms) {
    return timeout_ms != 0 &&
           std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - start).count() >= timeout_ms;
}

// Socket timeout wakeups are retryable TLS events; the caller owns the actual
// operation deadline.
int bio_send(void* context, const unsigned char* data, size_t len) {
    int fd = *static_cast<int*>(context);
    ssize_t result = ::send(fd, data, len, 0);
    if (result >= 0) {
        return static_cast<int>(result);
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

int bio_receive(void* context, unsigned char* data, size_t len) {
    int fd = *static_cast<int*>(context);
    ssize_t result = ::recv(fd, data, len, 0);
    if (result > 0) {
        return static_cast<int>(result);
    }
    if (result == 0) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

}  // namespace

struct TlsClientStream::Impl {
    int fd = -1;
    bool configured = false;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context private_key;

    Impl() {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&config);
        mbedtls_x509_crt_init(&certificate);
        mbedtls_pk_init(&private_key);
    }

    ~Impl() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&config);
        mbedtls_x509_crt_free(&certificate);
        mbedtls_pk_free(&private_key);
    }
};

TlsClientStream::TlsClientStream() : impl_(new Impl()) {}

TlsClientStream::~TlsClientStream() {
    delete impl_;
}

bool TlsClientStream::handshake(int fd, const RsaKey& key,
                                uint32_t timeout_ms) {
    if (impl_->configured || fd < 0) {
        return false;
    }

    std::vector<uint8_t> key_der;
    std::vector<uint8_t> certificate_der;
    if (!key.to_der(key_der)) {
        return false;
    }
    bool ready = key.self_signed_cert_der(certificate_der) &&
                 psa_crypto_init() == PSA_SUCCESS;
    int key_result = ready
                         ? mbedtls_pk_parse_key(
                               &impl_->private_key, key_der.data(),
                               key_der.size(), nullptr, 0)
                         : -1;
    mbedtls_platform_zeroize(key_der.data(), key_der.size());
    if (key_result != 0 ||
        mbedtls_x509_crt_parse_der(&impl_->certificate,
                                   certificate_der.data(),
                                   certificate_der.size()) != 0 ||
        mbedtls_ssl_config_defaults(&impl_->config, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return false;
    }

    // Android uses an ephemeral self-signed server certificate. Pairing binds
    // the channel with SPAKE2, while normal ADB authenticates our client key;
    // neither flow has a Web PKI chain to verify here.
    mbedtls_ssl_conf_authmode(&impl_->config, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_min_tls_version(&impl_->config,
                                     MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&impl_->config,
                                     MBEDTLS_SSL_VERSION_TLS1_3);
    if (mbedtls_ssl_conf_own_cert(&impl_->config, &impl_->certificate,
                                  &impl_->private_key) != 0 ||
        mbedtls_ssl_setup(&impl_->ssl, &impl_->config) != 0) {
        return false;
    }

    impl_->fd = fd;
    impl_->configured = true;
    mbedtls_ssl_set_bio(&impl_->ssl, &impl_->fd, bio_send, bio_receive,
                        nullptr);

    auto start = Clock::now();
    for (;;) {
        int result = mbedtls_ssl_handshake(&impl_->ssl);
        if (result == 0) {
            return true;
        }
        if (result != MBEDTLS_ERR_SSL_WANT_READ &&
            result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return false;
        }
        if (timed_out(start, timeout_ms)) {
            return false;
        }
    }
}

bool TlsClientStream::write_all(const uint8_t* data, size_t len,
                                uint32_t timeout_ms) {
    if (!impl_->configured || (!data && len != 0)) {
        return false;
    }
    auto start = Clock::now();
    size_t offset = 0;
    while (offset < len) {
        int result = mbedtls_ssl_write(&impl_->ssl, data + offset,
                                       len - offset);
        if (result > 0) {
            offset += static_cast<size_t>(result);
            continue;
        }
        if (result != MBEDTLS_ERR_SSL_WANT_READ &&
            result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return false;
        }
        if (timed_out(start, timeout_ms)) {
            return false;
        }
    }
    return true;
}

TlsIoResult TlsClientStream::read_exact(uint8_t* data, size_t len,
                                        bool idle_timeout,
                                        uint32_t timeout_ms) {
    if (!impl_->configured || (!data && len != 0)) {
        return TlsIoResult::Error;
    }
    auto start = Clock::now();
    size_t offset = 0;
    while (offset < len) {
        int result = mbedtls_ssl_read(&impl_->ssl, data + offset,
                                      len - offset);
        if (result > 0) {
            offset += static_cast<size_t>(result);
            continue;
        }
        // Mbed TLS surfaces a successfully processed TLS 1.3 ticket as a
        // non-data result; it is not a peer or transport failure.
        if (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;
        }
        if (result == MBEDTLS_ERR_SSL_WANT_READ ||
            result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (offset == 0 && idle_timeout) {
                return TlsIoResult::Timeout;
            }
            if (timed_out(start, timeout_ms)) {
                return TlsIoResult::Timeout;
            }
            continue;
        }
        return TlsIoResult::Error;
    }
    return TlsIoResult::Ok;
}

bool TlsClientStream::export_keying_material(
    const char* label, size_t label_len,
    uint8_t* output, size_t output_len) {
#if defined(MBEDTLS_SSL_KEYING_MATERIAL_EXPORT)
    return impl_->configured && label && output &&
           mbedtls_ssl_export_keying_material(
               &impl_->ssl, output, output_len, label, label_len,
               nullptr, 0, 0) == 0;
#else
    (void) label;
    (void) label_len;
    (void) output;
    (void) output_len;
    return false;
#endif
}

}  // namespace internal
}  // namespace adb
