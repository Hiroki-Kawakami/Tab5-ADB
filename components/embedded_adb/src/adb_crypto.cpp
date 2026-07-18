#include "adb_crypto.hpp"

#include <mbedtls/base64.h>
#include <psa/crypto.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

#include <cstring>

namespace adb {

namespace {

// Android RSA public-key wire format (adb's libcrypto_utils/android_pubkey.h).
constexpr size_t kModulusBytes = 2048 / 8;                       // 256
constexpr size_t kModulusWords = kModulusBytes / 4;             // 64
constexpr size_t kPubkeyEncodedSize = 3 * 4 + 2 * kModulusBytes;  // 524

bool read_der_length(const uint8_t*& cursor, const uint8_t* end, size_t& length) {
    if (cursor == end) return false;
    uint8_t first = *cursor++;
    if ((first & 0x80) == 0) {
        length = first;
        return length <= static_cast<size_t>(end - cursor);
    }
    size_t bytes = first & 0x7f;
    if (bytes == 0 || bytes > sizeof(size_t) || bytes > static_cast<size_t>(end - cursor)) {
        return false;
    }
    length = 0;
    for (size_t i = 0; i < bytes; ++i) length = (length << 8) | *cursor++;
    return length <= static_cast<size_t>(end - cursor);
}

bool read_der_integer(const uint8_t*& cursor, const uint8_t* end,
                      const uint8_t*& data, size_t& length) {
    if (cursor == end || *cursor++ != 0x02 || !read_der_length(cursor, end, length)) {
        return false;
    }
    while (length > 1 && *cursor == 0) {
        ++cursor;
        --length;
    }
    data = cursor;
    cursor += length;
    return true;
}

void store_u32_le(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

int compare_words(const uint32_t* lhs, const uint32_t* rhs) {
    for (size_t i = kModulusWords; i > 0; --i) {
        if (lhs[i - 1] < rhs[i - 1]) return -1;
        if (lhs[i - 1] > rhs[i - 1]) return 1;
    }
    return 0;
}

void subtract_words(uint32_t* lhs, const uint32_t* rhs) {
    uint64_t borrow = 0;
    for (size_t i = 0; i < kModulusWords; ++i) {
        uint64_t value = lhs[i];
        uint64_t subtrahend = static_cast<uint64_t>(rhs[i]) + borrow;
        lhs[i] = static_cast<uint32_t>(value - subtrahend);
        borrow = value < subtrahend;
    }
}

void double_mod(uint32_t* value, const uint32_t* modulus) {
    uint64_t carry = 0;
    for (size_t i = 0; i < kModulusWords; ++i) {
        uint64_t doubled = static_cast<uint64_t>(value[i]) * 2 + carry;
        value[i] = static_cast<uint32_t>(doubled);
        carry = doubled >> 32;
    }
    if (carry != 0 || compare_words(value, modulus) >= 0) subtract_words(value, modulus);
}

bool android_pubkey_encode(mbedtls_svc_key_id_t key,
                           uint8_t out[kPubkeyEncodedSize]) {
    uint8_t der[512];
    size_t der_len = 0;
    if (psa_export_public_key(key, der, sizeof(der), &der_len) != PSA_SUCCESS) return false;

    const uint8_t* cursor = der;
    const uint8_t* end = der + der_len;
    size_t sequence_len = 0;
    if (cursor == end || *cursor++ != 0x30 ||
        !read_der_length(cursor, end, sequence_len) ||
        sequence_len != static_cast<size_t>(end - cursor)) {
        return false;
    }

    const uint8_t* modulus_be = nullptr;
    const uint8_t* exponent_be = nullptr;
    size_t modulus_len = 0;
    size_t exponent_len = 0;
    if (!read_der_integer(cursor, end, modulus_be, modulus_len) ||
        !read_der_integer(cursor, end, exponent_be, exponent_len) ||
        cursor != end || modulus_len > kModulusBytes || exponent_len > 4) {
        return false;
    }

    uint32_t modulus[kModulusWords] = {};
    for (size_t i = 0; i < modulus_len; ++i) {
        size_t byte_index = modulus_len - 1 - i;
        modulus[i / 4] |= static_cast<uint32_t>(modulus_be[byte_index]) << ((i % 4) * 8);
    }

    uint32_t exponent = 0;
    for (size_t i = 0; i < exponent_len; ++i) exponent = (exponent << 8) | exponent_be[i];

    uint32_t inverse = 1;
    for (int i = 0; i < 5; ++i) inverse *= 2u - modulus[0] * inverse;

    uint32_t rr[kModulusWords] = {};
    rr[0] = 1;
    for (size_t i = 0; i < 2 * kModulusBytes * 8; ++i) double_mod(rr, modulus);

    std::memset(out, 0, kPubkeyEncodedSize);
    store_u32_le(out, static_cast<uint32_t>(kModulusWords));
    store_u32_le(out + 4, 0u - inverse);
    for (size_t i = 0; i < kModulusWords; ++i) {
        store_u32_le(out + 8 + i * 4, modulus[i]);
        store_u32_le(out + 8 + kModulusBytes + i * 4, rr[i]);
    }
    store_u32_le(out + 8 + 2 * kModulusBytes, exponent);
    return true;
}

}  // namespace

struct RsaKey::Impl {
    mbedtls_pk_context pk;
    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    bool key_valid = false;

    Impl() { mbedtls_pk_init(&pk); }
    ~Impl() {
        mbedtls_pk_free(&pk);
        if (key_valid) psa_destroy_key(key);
    }

    bool attach(mbedtls_svc_key_id_t value) {
        key = value;
        key_valid = true;
        return mbedtls_pk_wrap_psa(&pk, key) == 0;
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
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_KEY_PAIR);
    psa_set_key_bits(&attributes, 2048);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attributes,
                          PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_ANY_HASH));
    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    bool ok = psa_crypto_init() == PSA_SUCCESS &&
              psa_generate_key(&attributes, &key) == PSA_SUCCESS &&
              impl->attach(key);
    psa_reset_key_attributes(&attributes);
    if (!ok) {
        delete impl;
        return std::nullopt;
    }
    return RsaKey(impl);
}

std::optional<RsaKey> RsaKey::from_der(const uint8_t* der, size_t len) {
    auto impl = new Impl();
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_KEY_PAIR);
    psa_set_key_bits(&attributes, 2048);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attributes,
                          PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_ANY_HASH));
    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    bool ok = psa_crypto_init() == PSA_SUCCESS &&
              psa_import_key(&attributes, der, len, &key) == PSA_SUCCESS &&
              impl->attach(key);
    psa_reset_key_attributes(&attributes);
    if (!ok) {
        delete impl;
        return std::nullopt;
    }
    return RsaKey(impl);
}

bool RsaKey::to_der(std::vector<uint8_t>& out) const {
    uint8_t buf[2048];
    size_t len = 0;
    if (psa_export_key(impl_->key, buf, sizeof(buf), &len) != PSA_SUCCESS) return false;
    out.assign(buf, buf + len);
    return true;
}

bool RsaKey::sign_token(const uint8_t* token, size_t token_len,
                        std::vector<uint8_t>& sig) const {
    uint8_t buf[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t olen = 0;
    psa_status_t rc = psa_sign_hash(
        impl_->key, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_1), token, token_len,
        buf, sizeof(buf), &olen);
    if (rc != PSA_SUCCESS) return false;
    sig.assign(buf, buf + olen);
    return true;
}

bool RsaKey::android_public_key(std::string& out, const char* comment) const {
    uint8_t blob[kPubkeyEncodedSize];
    if (!android_pubkey_encode(impl_->key, blob)) return false;

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

        uint8_t buf[2048];
        int len = mbedtls_x509write_crt_der(&crt, buf, sizeof(buf));
        if (len < 0) break;
        out.assign(buf + sizeof(buf) - len, buf + sizeof(buf));
        ok = true;
    } while (false);

    mbedtls_x509write_crt_free(&crt);
    return ok;
}

}  // namespace adb
