// Standalone host test for adb_crypto (run on the desktop, no device needed).
// Validates the auth math before it is trusted against a real phone in P4:
//   - DER private-key round trip
//   - token signing verifies against the public key (PKCS#1 v1.5 + SHA1)
//   - the Android public-key blob has the correct structure and invariants
//     (modulus == N, exponent == 65537, n0inv == -1/N mod 2^32, rr == 2^4096 mod N)
//
// Build & run with the test runner (no device needed for this one):
//   nix develop -c components/embedded_adb/test/run.sh    # TEST=test_crypto (default)
// (See test/run.sh for the underlying g++ command if you need it by hand.)
#include "adb_crypto.hpp"

#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>

#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) {                                                 \
            std::printf("  ok   %s\n", msg);                        \
        } else {                                                    \
            std::printf("  FAIL %s\n", msg);                        \
            ++failures;                                             \
        }                                                           \
    } while (0)

// Recompute the Android-pubkey invariants directly from N to confirm the blob.
static bool verify_pubkey_blob(const std::string& pubkey, const mbedtls_mpi* N,
                               const mbedtls_mpi* E) {
    // strip " comment"
    size_t sp = pubkey.find(' ');
    std::string b64 = pubkey.substr(0, sp);

    uint8_t blob[600];
    size_t blob_len = 0;
    if (mbedtls_base64_decode(blob, sizeof(blob), &blob_len,
                              reinterpret_cast<const uint8_t*>(b64.data()),
                              b64.size()) != 0) {
        std::printf("  FAIL base64 decode\n");
        return false;
    }
    if (blob_len != 524) {
        std::printf("  FAIL blob length %zu != 524\n", blob_len);
        return false;
    }

    bool ok = true;
    uint32_t words = blob[0] | (blob[1] << 8) | (blob[2] << 16) | (blob[3] << 24);
    CHECK(words == 64, "modulus_size_words == 64");
    uint32_t exp;
    std::memcpy(&exp, blob + 520, 4);  // host is little-endian
    CHECK(exp == 65537, "exponent == 65537");

    // modulus (LE) round-trips to N
    mbedtls_mpi m;
    mbedtls_mpi_init(&m);
    mbedtls_mpi_read_binary_le(&m, blob + 8, 256);
    CHECK(mbedtls_mpi_cmp_mpi(&m, N) == 0, "blob modulus == key N");

    // n0inv: (N mod 2^32) * n0inv ≡ -1 (mod 2^32)
    mbedtls_mpi r32, n0, prod, n0inv;
    mbedtls_mpi_init(&r32);
    mbedtls_mpi_init(&n0);
    mbedtls_mpi_init(&prod);
    mbedtls_mpi_init(&n0inv);
    mbedtls_mpi_lset(&r32, 1);
    mbedtls_mpi_shift_l(&r32, 32);
    mbedtls_mpi_read_binary_le(&n0inv, blob + 4, 4);
    mbedtls_mpi_mod_mpi(&n0, N, &r32);
    mbedtls_mpi_mul_mpi(&prod, &n0, &n0inv);
    mbedtls_mpi_mod_mpi(&prod, &prod, &r32);
    mbedtls_mpi minus1;  // -1 mod 2^32 == 2^32 - 1
    mbedtls_mpi_init(&minus1);
    mbedtls_mpi_sub_int(&minus1, &r32, 1);
    CHECK(mbedtls_mpi_cmp_mpi(&prod, &minus1) == 0, "n0inv == -1/N mod 2^32");

    // rr == 2^4096 mod N
    mbedtls_mpi rr_blob, rr_calc, two4096;
    mbedtls_mpi_init(&rr_blob);
    mbedtls_mpi_init(&rr_calc);
    mbedtls_mpi_init(&two4096);
    mbedtls_mpi_read_binary_le(&rr_blob, blob + 264, 256);
    mbedtls_mpi_lset(&two4096, 1);
    mbedtls_mpi_shift_l(&two4096, 4096);
    mbedtls_mpi_mod_mpi(&rr_calc, &two4096, N);
    CHECK(mbedtls_mpi_cmp_mpi(&rr_blob, &rr_calc) == 0, "rr == 2^4096 mod N");

    (void)E;
    mbedtls_mpi_free(&m);
    mbedtls_mpi_free(&r32);
    mbedtls_mpi_free(&n0);
    mbedtls_mpi_free(&prod);
    mbedtls_mpi_free(&n0inv);
    mbedtls_mpi_free(&minus1);
    mbedtls_mpi_free(&rr_blob);
    mbedtls_mpi_free(&rr_calc);
    mbedtls_mpi_free(&two4096);
    return ok;
}

int main() {
    std::printf("adb_crypto self-test\n");

    auto key = adb::RsaKey::generate();
    CHECK(key.has_value(), "generate RSA-2048");
    if (!key) return 1;

    // DER round trip
    std::vector<uint8_t> der;
    CHECK(key->to_der(der) && !der.empty(), "to_der");
    auto key2 = adb::RsaKey::from_der(der.data(), der.size());
    CHECK(key2.has_value(), "from_der");

    // Sign a 20-byte token and verify with the public key.
    uint8_t token[20];
    for (int i = 0; i < 20; ++i) token[i] = static_cast<uint8_t>(i * 7 + 1);
    std::vector<uint8_t> sig;
    CHECK(key->sign_token(token, sizeof(token), sig), "sign_token");
    CHECK(sig.size() == 256, "signature is 256 bytes");

    // Reconstruct a public-only key from the Android blob's N/E and verify the
    // signature — this proves sign + pubkey are mutually consistent.
    std::string pubkey;
    CHECK(key->android_public_key(pubkey, "test@host"), "android_public_key");
    std::printf("  pubkey: %.40s...\n", pubkey.c_str());

    // Verify signature using the original private key's public part.
    // (Pull N, E from the DER-reloaded key via a fresh parse.)
    mbedtls_pk_context vpk;
    mbedtls_pk_init(&vpk);
    // parse the private key again to get a verifiable context
    CHECK(mbedtls_pk_parse_key(&vpk, der.data(), der.size(), nullptr, 0,
                               nullptr, nullptr) == 0, "reparse for verify");
    int vrc = mbedtls_pk_verify(&vpk, MBEDTLS_MD_SHA1, token, sizeof(token),
                                sig.data(), sig.size());
    CHECK(vrc == 0, "signature verifies against public key");

    // Validate the Android pubkey blob structure against N, E.
    mbedtls_mpi N, E;
    mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&E);
    mbedtls_rsa_export(mbedtls_pk_rsa(vpk), &N, nullptr, nullptr, nullptr, &E);
    verify_pubkey_blob(pubkey, &N, &E);
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&E);
    mbedtls_pk_free(&vpk);

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
