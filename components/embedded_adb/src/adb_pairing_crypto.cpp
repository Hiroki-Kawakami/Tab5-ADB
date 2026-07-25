#include "adb_pairing_crypto.hpp"

#include <mbedtls/platform_util.h>

#include <array>
#include <limits>

namespace adb {
namespace pairing {

namespace {

constexpr size_t kAesKeySize = 16;
constexpr size_t kGcmTagSize = 16;
// Unlike the SPAKE2 identities and TLS exporter label, this HKDF info excludes
// the C-string terminator.
constexpr uint8_t kHkdfInfo[] =
    "adb pairing_auth aes-128-gcm key";

}  // namespace

PairingCipher::~PairingCipher() {
    if (initialized_) {
        psa_destroy_key(key_);
    }
}

bool PairingCipher::init(const uint8_t* key_material,
                         size_t key_material_len) {
    if (initialized_ || !key_material || key_material_len == 0 ||
        psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }

    std::array<uint8_t, kAesKeySize> key_bytes{};
    psa_key_derivation_operation_t derivation =
        PSA_KEY_DERIVATION_OPERATION_INIT;
    bool ok = psa_key_derivation_setup(
                  &derivation,
                  PSA_ALG_HKDF(PSA_ALG_SHA_256)) == PSA_SUCCESS &&
              psa_key_derivation_input_bytes(
                  &derivation, PSA_KEY_DERIVATION_INPUT_SECRET,
                  key_material, key_material_len) == PSA_SUCCESS &&
              psa_key_derivation_input_bytes(
                  &derivation, PSA_KEY_DERIVATION_INPUT_INFO,
                  kHkdfInfo, sizeof(kHkdfInfo) - 1) == PSA_SUCCESS &&
              psa_key_derivation_output_bytes(
                  &derivation, key_bytes.data(),
                  key_bytes.size()) == PSA_SUCCESS;
    psa_key_derivation_abort(&derivation);
    if (!ok) {
        mbedtls_platform_zeroize(key_bytes.data(), key_bytes.size());
        return false;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, kAesKeySize * 8);
    psa_set_key_usage_flags(&attributes,
                            PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    ok = psa_import_key(&attributes, key_bytes.data(), key_bytes.size(),
                        &key_) == PSA_SUCCESS;
    psa_reset_key_attributes(&attributes);
    mbedtls_platform_zeroize(key_bytes.data(), key_bytes.size());
    initialized_ = ok;
    return ok;
}

size_t PairingCipher::encrypted_size(size_t plaintext_len) const {
    if (plaintext_len > std::numeric_limits<size_t>::max() - kGcmTagSize) {
        return 0;
    }
    return plaintext_len + kGcmTagSize;
}

size_t PairingCipher::decrypted_size(size_t ciphertext_len) const {
    return ciphertext_len >= kGcmTagSize
               ? ciphertext_len - kGcmTagSize
               : 0;
}

void PairingCipher::make_nonce(uint64_t sequence, uint8_t nonce[12]) {
    // Android puts the little-endian 64-bit sequence in the first eight bytes;
    // the remaining four bytes stay zero.
    for (size_t i = 0; i < 8; ++i) {
        nonce[i] = static_cast<uint8_t>(sequence);
        sequence >>= 8;
    }
    for (size_t i = 8; i < 12; ++i) {
        nonce[i] = 0;
    }
}

bool PairingCipher::encrypt(const uint8_t* plaintext, size_t plaintext_len,
                            uint8_t* ciphertext,
                            size_t ciphertext_capacity,
                            size_t& ciphertext_len) {
    ciphertext_len = 0;
    size_t required = encrypted_size(plaintext_len);
    if (!initialized_ || !plaintext || plaintext_len == 0 || !ciphertext ||
        required == 0 || ciphertext_capacity < required ||
        encrypt_sequence_ == std::numeric_limits<uint64_t>::max()) {
        return false;
    }
    uint8_t nonce[12];
    make_nonce(encrypt_sequence_, nonce);
    psa_status_t status = psa_aead_encrypt(
        key_, PSA_ALG_GCM, nonce, sizeof(nonce), nullptr, 0,
        plaintext, plaintext_len, ciphertext, ciphertext_capacity,
        &ciphertext_len);
    if (status != PSA_SUCCESS) {
        ciphertext_len = 0;
        return false;
    }
    ++encrypt_sequence_;
    return true;
}

bool PairingCipher::decrypt(const uint8_t* ciphertext, size_t ciphertext_len,
                            uint8_t* plaintext,
                            size_t plaintext_capacity,
                            size_t& plaintext_len) {
    plaintext_len = 0;
    size_t required = decrypted_size(ciphertext_len);
    if (!initialized_ || !ciphertext || required == 0 || !plaintext ||
        plaintext_capacity < required ||
        decrypt_sequence_ == std::numeric_limits<uint64_t>::max()) {
        return false;
    }
    uint8_t nonce[12];
    make_nonce(decrypt_sequence_, nonce);
    psa_status_t status = psa_aead_decrypt(
        key_, PSA_ALG_GCM, nonce, sizeof(nonce), nullptr, 0,
        ciphertext, ciphertext_len, plaintext, plaintext_capacity,
        &plaintext_len);
    if (status != PSA_SUCCESS) {
        plaintext_len = 0;
        return false;
    }
    ++decrypt_sequence_;
    return true;
}

}  // namespace pairing
}  // namespace adb
