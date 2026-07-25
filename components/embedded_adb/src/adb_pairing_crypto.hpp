// Record protection used after the pairing SPAKE2 exchange. Android derives one
// AES-128-GCM key and keeps independent sequence numbers per direction.
#pragma once

#include <cstddef>
#include <cstdint>

#include <psa/crypto.h>

namespace adb {
namespace pairing {

class PairingCipher {
public:
    PairingCipher() = default;
    ~PairingCipher();

    PairingCipher(const PairingCipher&) = delete;
    PairingCipher& operator=(const PairingCipher&) = delete;

    // key_material is the 64-byte SPAKE2 transcript result.
    bool init(const uint8_t* key_material, size_t key_material_len);
    size_t encrypted_size(size_t plaintext_len) const;
    size_t decrypted_size(size_t ciphertext_len) const;
    bool encrypt(const uint8_t* plaintext, size_t plaintext_len,
                 uint8_t* ciphertext, size_t ciphertext_capacity,
                 size_t& ciphertext_len);
    bool decrypt(const uint8_t* ciphertext, size_t ciphertext_len,
                 uint8_t* plaintext, size_t plaintext_capacity,
                 size_t& plaintext_len);

private:
    static void make_nonce(uint64_t sequence, uint8_t nonce[12]);

    mbedtls_svc_key_id_t key_ = MBEDTLS_SVC_KEY_ID_INIT;
    bool initialized_ = false;
    uint64_t encrypt_sequence_ = 0;
    uint64_t decrypt_sequence_ = 0;
};

}  // namespace pairing
}  // namespace adb
