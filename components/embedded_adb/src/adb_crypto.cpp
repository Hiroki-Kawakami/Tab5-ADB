#include "adb_crypto.hpp"

#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/x509_crt.h>

#include <cstring>

namespace adb {

namespace {

// Android RSA public-key wire format (adb's libcrypto_utils/android_pubkey.h).
constexpr size_t kModulusBytes = 2048 / 8;                       // 256
constexpr size_t kModulusWords = kModulusBytes / 4;             // 64
constexpr size_t kPubkeyEncodedSize = 3 * 4 + 2 * kModulusBytes;  // 524

// Encode the RSA public key (N, e) into the 524-byte little-endian RSAPublicKey
// struct the device expects:
//   uint32 modulus_size_words | uint32 n0inv | u8 modulus[256] (LE)
//   u8 rr[256] (LE) | uint32 exponent
// n0inv = -1/n mod 2^32 ; rr = (2^2048)^2 mod n. All multibyte fields little-
// endian, so mbedtls_mpi_write_binary_le writes the uint32 fields directly too.
bool android_pubkey_encode(mbedtls_rsa_context* rsa, uint8_t out[kPubkeyEncodedSize]) {
    mbedtls_mpi N, E, r32, n0inv, rr;
    mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&E);
    mbedtls_mpi_init(&r32);
    mbedtls_mpi_init(&n0inv);
    mbedtls_mpi_init(&rr);

    bool ok = false;
    do {
        if (mbedtls_rsa_export(rsa, &N, nullptr, nullptr, nullptr, &E) != 0) break;

        std::memset(out, 0, kPubkeyEncodedSize);
        out[0] = static_cast<uint8_t>(kModulusWords);  // 64, LE in byte 0

        // r32 = 2^32 ; n0inv = (2^32 - (N^-1 mod 2^32)) mod 2^32
        if (mbedtls_mpi_lset(&r32, 1) != 0) break;
        if (mbedtls_mpi_shift_l(&r32, 32) != 0) break;
        if (mbedtls_mpi_mod_mpi(&n0inv, &N, &r32) != 0) break;     // N mod 2^32
        if (mbedtls_mpi_inv_mod(&n0inv, &n0inv, &r32) != 0) break;  // inverse
        if (mbedtls_mpi_sub_mpi(&n0inv, &r32, &n0inv) != 0) break;  // negate
        if (mbedtls_mpi_write_binary_le(&n0inv, out + 4, 4) != 0) break;

        // modulus (LE)
        if (mbedtls_mpi_write_binary_le(&N, out + 8, kModulusBytes) != 0) break;

        // rr = (2^2048)^2 mod N = 2^4096 mod N
        if (mbedtls_mpi_lset(&rr, 1) != 0) break;
        if (mbedtls_mpi_shift_l(&rr, 2 * kModulusBytes * 8) != 0) break;  // 2^4096
        if (mbedtls_mpi_mod_mpi(&rr, &rr, &N) != 0) break;
        if (mbedtls_mpi_write_binary_le(&rr, out + 8 + kModulusBytes, kModulusBytes) != 0) break;

        // exponent
        if (mbedtls_mpi_write_binary_le(&E, out + 8 + 2 * kModulusBytes, 4) != 0) break;

        ok = true;
    } while (false);

    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&r32);
    mbedtls_mpi_free(&n0inv);
    mbedtls_mpi_free(&rr);
    return ok;
}

}  // namespace

// Holds the parsed key plus a per-key DRBG (RSA private ops need an RNG for
// blinding; keygen needs it too).
struct RsaKey::Impl {
    mbedtls_pk_context pk;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;

    Impl() {
        mbedtls_pk_init(&pk);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
    }
    ~Impl() {
        mbedtls_pk_free(&pk);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }

    bool seed() {
        static const char kPers[] = "embedded_adb";
        return mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                     reinterpret_cast<const unsigned char*>(kPers),
                                     sizeof(kPers) - 1) == 0;
    }
};

RsaKey::RsaKey(RsaKey&& o) noexcept : impl_(o.impl_) { o.impl_ = nullptr; }

RsaKey& RsaKey::operator=(RsaKey&& o) noexcept {
    if (this != &o) {
        delete impl_;
        impl_ = o.impl_;
        o.impl_ = nullptr;
    }
    return *this;
}

RsaKey::~RsaKey() { delete impl_; }

std::optional<RsaKey> RsaKey::generate() {
    auto impl = new Impl();
    bool ok = impl->seed() &&
              mbedtls_pk_setup(&impl->pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) == 0 &&
              mbedtls_rsa_gen_key(mbedtls_pk_rsa(impl->pk), mbedtls_ctr_drbg_random,
                                  &impl->drbg, 2048, 65537) == 0;
    if (!ok) {
        delete impl;
        return std::nullopt;
    }
    return RsaKey(impl);
}

std::optional<RsaKey> RsaKey::from_der(const uint8_t* der, size_t len) {
    auto impl = new Impl();
    bool ok = impl->seed() &&
              mbedtls_pk_parse_key(&impl->pk, der, len, nullptr, 0,
                                   mbedtls_ctr_drbg_random, &impl->drbg) == 0;
    if (!ok) {
        delete impl;
        return std::nullopt;
    }
    return RsaKey(impl);
}

bool RsaKey::to_der(std::vector<uint8_t>& out) const {
    // pk_write_key_der writes to the END of the buffer and returns the length.
    uint8_t buf[2048];
    int len = mbedtls_pk_write_key_der(&impl_->pk, buf, sizeof(buf));
    if (len < 0) return false;
    out.assign(buf + sizeof(buf) - len, buf + sizeof(buf));
    return true;
}

bool RsaKey::sign_token(const uint8_t* token, size_t token_len,
                        std::vector<uint8_t>& sig) const {
    uint8_t buf[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t olen = 0;
    int rc = mbedtls_pk_sign(&impl_->pk, MBEDTLS_MD_SHA1, token, token_len, buf,
                             sizeof(buf), &olen, mbedtls_ctr_drbg_random, &impl_->drbg);
    if (rc != 0) return false;
    sig.assign(buf, buf + olen);
    return true;
}

bool RsaKey::android_public_key(std::string& out, const char* comment) const {
    uint8_t blob[kPubkeyEncodedSize];
    if (!android_pubkey_encode(mbedtls_pk_rsa(impl_->pk), blob)) return false;

    uint8_t b64[4 * ((kPubkeyEncodedSize + 2) / 3) + 1];
    size_t olen = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &olen, blob, sizeof(blob)) != 0) {
        return false;
    }
    out.assign(reinterpret_cast<char*>(b64), olen);
    out.append(" ");
    out.append(comment);
    return true;
}

bool RsaKey::self_signed_cert_der(std::vector<uint8_t>& out) const {
    constexpr char kName[] = "CN=Tab5 ADB,O=Tab5 ADB,C=US";
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);

    bool ok = false;
    do {
        // Self-signed: subject key == issuer key == our key.
        mbedtls_x509write_crt_set_subject_key(&crt, &impl_->pk);
        mbedtls_x509write_crt_set_issuer_key(&crt, &impl_->pk);
        if (mbedtls_x509write_crt_set_subject_name(&crt, kName) != 0) break;
        if (mbedtls_x509write_crt_set_issuer_name(&crt, kName) != 0) break;
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        // serial_raw (the non-deprecated form; the mpi set_serial is removed in the
        // ESP-IDF mbedTLS build).
        uint8_t serial[] = {0x01};
        if (mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0)
            break;
        // Fixed wide validity — the device has no RTC, and adbd matches the cert by
        // its public-key fingerprint, not its dates.
        if (mbedtls_x509write_crt_set_validity(&crt, "20200101000000",
                                               "20400101000000") != 0) {
            break;
        }

        // crt_der writes to the END of the buffer and returns the length.
        uint8_t buf[2048];
        int len = mbedtls_x509write_crt_der(&crt, buf, sizeof(buf),
                                            mbedtls_ctr_drbg_random, &impl_->drbg);
        if (len < 0) break;
        out.assign(buf + sizeof(buf) - len, buf + sizeof(buf));
        ok = true;
    } while (false);

    mbedtls_x509write_crt_free(&crt);
    return ok;
}

}  // namespace adb
