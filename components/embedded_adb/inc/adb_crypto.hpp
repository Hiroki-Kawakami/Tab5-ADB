// ADB host-side RSA authentication crypto. mbedTLS on both targets (ESP-IDF
// bundles it; the simulator links Nix's). No ESP-IDF or board dependency, so it
// is shared and unsplit. Mirrors the upstream auth math (adb/crypto + adb's
// libcrypto_utils/android_pubkey), reimplemented on mbedTLS.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace adb {

// RSA-2048 key used to authenticate to a device. Non-copyable (owns mbedTLS
// state); movable.
class RsaKey {
public:
    RsaKey(RsaKey&&) noexcept;
    RsaKey& operator=(RsaKey&&) noexcept;
    RsaKey(const RsaKey&) = delete;
    RsaKey& operator=(const RsaKey&) = delete;
    ~RsaKey();

    // Generate a fresh RSA-2048 key (public exponent 65537).
    static std::optional<RsaKey> generate();

    // Parse a private key previously produced by to_der().
    static std::optional<RsaKey> from_der(const uint8_t* der, size_t len);

    // Serialize the private key as PKCS#1/PKCS#8 DER (for NVS persistence).
    bool to_der(std::vector<uint8_t>& out) const;

    // Sign a 20-byte ADB AUTH token: PKCS#1 v1.5 over the token treated as a
    // SHA1 digest (matches upstream RSA_sign(NID_sha1, ...)). Output is 256 bytes
    // — the payload for an A_AUTH/ADB_AUTH_SIGNATURE packet.
    bool sign_token(const uint8_t* token, size_t token_len,
                    std::vector<uint8_t>& sig) const;

    // The Android public-key blob: base64 of the 524-byte RSAPublicKey struct,
    // followed by " <comment>" — the payload for an A_AUTH/ADB_AUTH_RSAPUBLICKEY
    // packet. The comment is what the device shows in its "Allow USB debugging?"
    // fingerprint dialog.
    bool android_public_key(std::string& out,
                            const char* comment = "tab5-adb@embedded") const;

private:
    struct Impl;
    explicit RsaKey(Impl* impl) : impl_(impl) {}
    Impl* impl_;
};

}  // namespace adb
