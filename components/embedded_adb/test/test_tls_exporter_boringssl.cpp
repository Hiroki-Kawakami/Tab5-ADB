#include "adb_crypto.hpp"
#include "adb_tls_stream.hpp"

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <thread>
#include <vector>

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

struct ServerResult {
    bool ok = false;
    std::array<uint8_t, 64> exporter{};
};

bool set_timeout(int fd) {
    timeval timeout{5, 0};
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                      &timeout, sizeof(timeout)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                      &timeout, sizeof(timeout)) == 0;
}

SSL_CTX* make_server_context(const adb::RsaKey& key) {
    std::vector<uint8_t> key_der;
    std::vector<uint8_t> certificate_der;
    if (!key.to_der(key_der) ||
        !key.self_signed_cert_der(certificate_der)) {
        return nullptr;
    }

    const uint8_t* key_ptr = key_der.data();
    EVP_PKEY* private_key = d2i_AutoPrivateKey(
        nullptr, &key_ptr, key_der.size());
    const uint8_t* certificate_ptr = certificate_der.data();
    X509* certificate = d2i_X509(
        nullptr, &certificate_ptr, certificate_der.size());
    SSL_CTX* context = SSL_CTX_new(TLS_method());
    bool ok = private_key && certificate && context &&
              SSL_CTX_set_min_proto_version(
                  context, TLS1_3_VERSION) == 1 &&
              SSL_CTX_set_max_proto_version(
                  context, TLS1_3_VERSION) == 1 &&
              SSL_CTX_use_PrivateKey(context, private_key) == 1 &&
              SSL_CTX_use_certificate(context, certificate) == 1;
    EVP_PKEY_free(private_key);
    X509_free(certificate);
    if (!ok) {
        SSL_CTX_free(context);
        return nullptr;
    }
    return context;
}

void run_server(SSL_CTX* context, int fd, ServerResult& result) {
    static constexpr char kExporterLabel[] = "adb-label";
    SSL* ssl = SSL_new(context);
    result.ok = ssl && SSL_set_fd(ssl, fd) == 1 &&
                SSL_accept(ssl) == 1 &&
                SSL_export_keying_material(
                    ssl, result.exporter.data(), result.exporter.size(),
                    kExporterLabel, sizeof(kExporterLabel),
                    nullptr, 0, 0) == 1;
    SSL_free(ssl);
    close(fd);
}

}  // namespace

int main() {
    std::printf("ADB TLS exporter BoringSSL interoperability test\n");

    auto server_key = adb::RsaKey::generate();
    auto client_key = adb::RsaKey::generate();
    CHECK(server_key.has_value(), "generate server RSA key");
    CHECK(client_key.has_value(), "generate client RSA key");
    if (!server_key || !client_key) {
        return 1;
    }

    SSL_CTX* server_context = make_server_context(*server_key);
    CHECK(server_context != nullptr, "configure BoringSSL TLS 1.3 server");
    if (!server_context) {
        return 1;
    }

    int sockets[2];
    bool sockets_ready = socketpair(
        AF_UNIX, SOCK_STREAM, 0, sockets) == 0;
    CHECK(sockets_ready, "create local socket pair");
    if (!sockets_ready) {
        SSL_CTX_free(server_context);
        return 1;
    }
    CHECK(set_timeout(sockets[0]) && set_timeout(sockets[1]),
          "set socket timeouts");

    ServerResult server_result;
    std::thread server(
        run_server, server_context, sockets[1],
        std::ref(server_result));

    static constexpr char kExporterLabel[] = "adb-label";
    std::array<uint8_t, 64> client_exporter{};
    bool client_ok = false;
    {
        adb::internal::TlsClientStream client;
        client_ok = client.handshake(sockets[0], *client_key, 5000) &&
                    client.export_keying_material(
                        kExporterLabel, sizeof(kExporterLabel),
                        client_exporter.data(), client_exporter.size());
    }
    shutdown(sockets[0], SHUT_RDWR);
    close(sockets[0]);
    server.join();
    SSL_CTX_free(server_context);

    CHECK(client_ok, "Mbed TLS client exports keying material");
    CHECK(server_result.ok, "BoringSSL server exports keying material");
    CHECK(client_exporter == server_result.exporter,
          "exporter output matches with terminating NUL");

    std::printf("%s (%d failures)\n",
                failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
