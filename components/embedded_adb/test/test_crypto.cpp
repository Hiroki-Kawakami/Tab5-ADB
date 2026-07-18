#include "adb_crypto.hpp"

#include <mbedtls/base64.h>
#include <psa/crypto.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) {                                                 \
            std::printf("  ok   %s\n", msg);                       \
        } else {                                                    \
            std::printf("  FAIL %s\n", msg);                       \
            ++failures;                                             \
        }                                                           \
    } while (0)

constexpr size_t kModulusBytes = 256;
constexpr size_t kModulusWords = kModulusBytes / 4;

static bool read_der_length(const uint8_t*& cursor, const uint8_t* end,
                            size_t& length) {
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

static bool read_der_integer(const uint8_t*& cursor, const uint8_t* end,
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

static uint32_t load_u32_le(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

static int compare_words(const uint32_t* lhs, const uint32_t* rhs) {
    for (size_t i = kModulusWords; i > 0; --i) {
        if (lhs[i - 1] < rhs[i - 1]) return -1;
        if (lhs[i - 1] > rhs[i - 1]) return 1;
    }
    return 0;
}

static void subtract_words(uint32_t* lhs, const uint32_t* rhs) {
    uint64_t borrow = 0;
    for (size_t i = 0; i < kModulusWords; ++i) {
        uint64_t value = lhs[i];
        uint64_t subtrahend = static_cast<uint64_t>(rhs[i]) + borrow;
        lhs[i] = static_cast<uint32_t>(value - subtrahend);
        borrow = value < subtrahend;
    }
}

static void double_mod(uint32_t* value, const uint32_t* modulus) {
    uint64_t carry = 0;
    for (size_t i = 0; i < kModulusWords; ++i) {
        uint64_t doubled = static_cast<uint64_t>(value[i]) * 2 + carry;
        value[i] = static_cast<uint32_t>(doubled);
        carry = doubled >> 32;
    }
    if (carry != 0 || compare_words(value, modulus) >= 0) subtract_words(value, modulus);
}

static bool export_public_values(mbedtls_svc_key_id_t key,
                                 std::vector<uint8_t>& modulus,
                                 uint32_t& exponent) {
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

    const uint8_t* modulus_data = nullptr;
    const uint8_t* exponent_data = nullptr;
    size_t modulus_len = 0;
    size_t exponent_len = 0;
    if (!read_der_integer(cursor, end, modulus_data, modulus_len) ||
        !read_der_integer(cursor, end, exponent_data, exponent_len) ||
        cursor != end || modulus_len > kModulusBytes || exponent_len > 4) {
        return false;
    }

    modulus.assign(modulus_data, modulus_data + modulus_len);
    exponent = 0;
    for (size_t i = 0; i < exponent_len; ++i) {
        exponent = (exponent << 8) | exponent_data[i];
    }
    return true;
}

static bool verify_pubkey_blob(const std::string& pubkey,
                               const std::vector<uint8_t>& modulus,
                               uint32_t exponent) {
    size_t separator = pubkey.find(' ');
    if (separator == std::string::npos) return false;

    std::string base64 = pubkey.substr(0, separator);
    uint8_t blob[600];
    size_t blob_len = 0;
    if (mbedtls_base64_decode(blob, sizeof(blob), &blob_len,
                              reinterpret_cast<const uint8_t*>(base64.data()),
                              base64.size()) != 0 || blob_len != 524) {
        return false;
    }

    CHECK(load_u32_le(blob) == kModulusWords, "modulus_size_words == 64");
    CHECK(load_u32_le(blob + 520) == exponent, "exponent matches key");

    bool modulus_matches = modulus.size() <= kModulusBytes;
    for (size_t i = 0; modulus_matches && i < kModulusBytes; ++i) {
        uint8_t expected = i < modulus.size() ? modulus[modulus.size() - 1 - i] : 0;
        modulus_matches = blob[8 + i] == expected;
    }
    CHECK(modulus_matches, "blob modulus matches key");

    uint32_t n0 = load_u32_le(blob + 8);
    uint32_t n0inv = load_u32_le(blob + 4);
    CHECK(static_cast<uint32_t>(static_cast<uint64_t>(n0) * n0inv) == UINT32_MAX,
          "n0inv == -1/N mod 2^32");

    uint32_t modulus_words[kModulusWords] = {};
    for (size_t i = 0; i < modulus.size(); ++i) {
        size_t byte_index = modulus.size() - 1 - i;
        modulus_words[i / 4] |=
            static_cast<uint32_t>(modulus[byte_index]) << ((i % 4) * 8);
    }
    uint32_t rr[kModulusWords] = {};
    rr[0] = 1;
    for (size_t i = 0; i < 4096; ++i) double_mod(rr, modulus_words);

    bool rr_matches = true;
    for (size_t i = 0; rr_matches && i < kModulusWords; ++i) {
        rr_matches = load_u32_le(blob + 264 + i * 4) == rr[i];
    }
    CHECK(rr_matches, "rr == 2^4096 mod N");
    return true;
}

int main() {
    std::printf("adb_crypto self-test\n");
    CHECK(psa_crypto_init() == PSA_SUCCESS, "psa_crypto_init");

    auto key = adb::RsaKey::generate();
    CHECK(key.has_value(), "generate RSA-2048");
    if (!key) return 1;

    std::vector<uint8_t> der;
    CHECK(key->to_der(der) && !der.empty(), "to_der");
    auto key2 = adb::RsaKey::from_der(der.data(), der.size());
    CHECK(key2.has_value(), "from_der");

    uint8_t token[20];
    for (int i = 0; i < 20; ++i) token[i] = static_cast<uint8_t>(i * 7 + 1);
    std::vector<uint8_t> signature;
    CHECK(key->sign_token(token, sizeof(token), signature), "sign_token");
    CHECK(signature.size() == 256, "signature is 256 bytes");

    std::string pubkey;
    CHECK(key->android_public_key(pubkey, "test@host"), "android_public_key");

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_KEY_PAIR);
    psa_set_key_bits(&attributes, 2048);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes,
                          PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_1));
    mbedtls_svc_key_id_t verify_key = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t import_status = psa_import_key(&attributes, der.data(), der.size(),
                                                &verify_key);
    psa_reset_key_attributes(&attributes);
    CHECK(import_status == PSA_SUCCESS, "import for verify");
    if (import_status == PSA_SUCCESS) {
        CHECK(psa_verify_hash(verify_key,
                              PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_1),
                              token, sizeof(token), signature.data(),
                              signature.size()) == PSA_SUCCESS,
              "signature verifies against public key");
        std::vector<uint8_t> modulus;
        uint32_t exponent = 0;
        CHECK(export_public_values(verify_key, modulus, exponent),
              "export public key");
        if (!modulus.empty()) {
            CHECK(verify_pubkey_blob(pubkey, modulus, exponent),
                  "Android public key blob parses");
        }
        psa_destroy_key(verify_key);
    }

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
