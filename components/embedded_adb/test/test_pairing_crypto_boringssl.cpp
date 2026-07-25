#include "adb_pairing_crypto.hpp"

#include <openssl/evp.h>
#include <openssl/hkdf.h>
#include <psa/crypto.h>

#include <array>
#include <cstdio>
#include <cstring>
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

bool reference_encrypt(const uint8_t* material, size_t material_len,
                       uint64_t sequence, const uint8_t* plaintext,
                       size_t plaintext_len,
                       std::vector<uint8_t>& ciphertext) {
    static constexpr uint8_t kInfo[] =
        "adb pairing_auth aes-128-gcm key";
    std::array<uint8_t, 16> key{};
    if (HKDF(key.data(), key.size(), EVP_sha256(), material, material_len,
             nullptr, 0, kInfo, sizeof(kInfo) - 1) != 1) {
        return false;
    }

    bssl::ScopedEVP_AEAD_CTX context;
    if (!EVP_AEAD_CTX_init(context.get(), EVP_aead_aes_128_gcm(),
                           key.data(), key.size(),
                           EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr)) {
        return false;
    }
    std::array<uint8_t, 12> nonce{};
    for (size_t i = 0; i < 8; ++i) {
        nonce[i] = static_cast<uint8_t>(sequence);
        sequence >>= 8;
    }
    ciphertext.resize(
        plaintext_len + EVP_AEAD_max_overhead(EVP_aead_aes_128_gcm()));
    size_t output_len = 0;
    if (!EVP_AEAD_CTX_seal(
            context.get(), ciphertext.data(), &output_len,
            ciphertext.size(), nonce.data(), nonce.size(),
            plaintext, plaintext_len, nullptr, 0)) {
        return false;
    }
    ciphertext.resize(output_len);
    return true;
}

}  // namespace

int main() {
    std::printf("adb_pairing_crypto BoringSSL interoperability test\n");
    CHECK(psa_crypto_init() == PSA_SUCCESS, "psa_crypto_init");

    std::array<uint8_t, 64> material{};
    for (size_t i = 0; i < material.size(); ++i) {
        material[i] = static_cast<uint8_t>(i * 11 + 3);
    }
    std::array<uint8_t, 257> plaintext{};
    for (size_t i = 0; i < plaintext.size(); ++i) {
        plaintext[i] = static_cast<uint8_t>(i * 7 + 9);
    }

    adb::pairing::PairingCipher cipher;
    CHECK(cipher.init(material.data(), material.size()), "initialize cipher");

    std::vector<uint8_t> expected;
    CHECK(reference_encrypt(material.data(), material.size(), 0,
                            plaintext.data(), plaintext.size(), expected),
          "BoringSSL encrypt sequence 0");
    std::vector<uint8_t> actual(cipher.encrypted_size(plaintext.size()));
    size_t actual_len = 0;
    CHECK(cipher.encrypt(plaintext.data(), plaintext.size(), actual.data(),
                         actual.size(), actual_len),
          "PSA encrypt sequence 0");
    actual.resize(actual_len);
    CHECK(actual == expected, "sequence 0 ciphertext and tag match");

    CHECK(reference_encrypt(material.data(), material.size(), 1,
                            plaintext.data(), plaintext.size(), expected),
          "BoringSSL encrypt sequence 1");
    actual.assign(cipher.encrypted_size(plaintext.size()), 0);
    CHECK(cipher.encrypt(plaintext.data(), plaintext.size(), actual.data(),
                         actual.size(), actual_len),
          "PSA encrypt sequence 1");
    actual.resize(actual_len);
    CHECK(actual == expected, "sequence 1 ciphertext and tag match");

    adb::pairing::PairingCipher decryptor;
    CHECK(decryptor.init(material.data(), material.size()),
          "initialize decryptor");
    CHECK(reference_encrypt(material.data(), material.size(), 0,
                            plaintext.data(), plaintext.size(), expected),
          "prepare decrypt ciphertext");
    std::vector<uint8_t> decrypted(
        decryptor.decrypted_size(expected.size()));
    size_t decrypted_len = 0;
    CHECK(decryptor.decrypt(expected.data(), expected.size(),
                            decrypted.data(), decrypted.size(),
                            decrypted_len),
          "decrypt BoringSSL ciphertext");
    decrypted.resize(decrypted_len);
    CHECK(decrypted.size() == plaintext.size() &&
              std::memcmp(decrypted.data(), plaintext.data(),
                          plaintext.size()) == 0,
          "decrypted plaintext matches");

    expected.back() ^= 0x80;
    adb::pairing::PairingCipher tampered;
    CHECK(tampered.init(material.data(), material.size()),
          "initialize tamper test");
    decrypted.assign(plaintext.size(), 0);
    CHECK(!tampered.decrypt(expected.data(), expected.size(),
                            decrypted.data(), decrypted.size(),
                            decrypted_len),
          "reject modified authentication tag");

    std::printf("%s (%d failures)\n",
                failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
