#include "adb_spake2.hpp"

#include <openssl/curve25519.h>
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

std::array<uint8_t, 64> make_random(uint8_t seed) {
    std::array<uint8_t, 64> random{};
    for (size_t i = 0; i < random.size(); ++i) {
        random[i] = static_cast<uint8_t>(seed + i * 17);
    }
    return random;
}

bool run_case(adb::pairing::Spake2Role our_role) {
    static constexpr uint8_t kClientName[] = "adb pair client";
    static constexpr uint8_t kServerName[] = "adb pair server";
    std::vector<uint8_t> password{'1', '2', '3', '4', '5', '6', 0x00, 0x91,
                                  0x4a, 0xff, 0x23, 0x00, 0x68};

    spake2_role_t bssl_role;
    const uint8_t* my_name;
    size_t my_name_len;
    const uint8_t* peer_name;
    size_t peer_name_len;
    if (our_role == adb::pairing::Spake2Role::Alice) {
        bssl_role = spake2_role_bob;
        my_name = kServerName;
        my_name_len = sizeof(kServerName);
        peer_name = kClientName;
        peer_name_len = sizeof(kClientName);
    } else {
        bssl_role = spake2_role_alice;
        my_name = kClientName;
        my_name_len = sizeof(kClientName);
        peer_name = kServerName;
        peer_name_len = sizeof(kServerName);
    }

    SPAKE2_CTX* reference = SPAKE2_CTX_new(
        bssl_role, my_name, my_name_len, peer_name, peer_name_len);
    if (!reference) {
        return false;
    }
    std::array<uint8_t, SPAKE2_MAX_MSG_SIZE> reference_message{};
    size_t reference_message_len = 0;
    bool ok = SPAKE2_generate_msg(
                  reference, reference_message.data(),
                  &reference_message_len, reference_message.size(),
                  password.data(), password.size()) == 1 &&
              reference_message_len == 32;

    auto random = make_random(
        our_role == adb::pairing::Spake2Role::Alice ? 19 : 173);
    adb::pairing::Spake2 ours(our_role);
    ok = ok && ours.init(password.data(), password.size(), random.data());

    std::array<uint8_t, SPAKE2_MAX_KEY_SIZE> reference_key{};
    size_t reference_key_len = 0;
    if (ok) {
        ok = SPAKE2_process_msg(
                 reference, reference_key.data(), &reference_key_len,
                 reference_key.size(), ours.message().data(),
                 ours.message().size()) == 1 &&
             reference_key_len == 64;
    }

    std::array<uint8_t, 64> our_key{};
    if (ok) {
        ok = ours.process_message(reference_message.data(),
                                  reference_message_len, our_key) &&
             std::memcmp(our_key.data(), reference_key.data(),
                         our_key.size()) == 0;
    }
    SPAKE2_CTX_free(reference);
    return ok;
}

}  // namespace

int main() {
    std::printf("adb_spake2 BoringSSL interoperability test\n");
    CHECK(psa_crypto_init() == PSA_SUCCESS, "psa_crypto_init");
    CHECK(run_case(adb::pairing::Spake2Role::Alice),
          "our Alice matches BoringSSL Bob");
    CHECK(run_case(adb::pairing::Spake2Role::Bob),
          "our Bob matches BoringSSL Alice");
    std::printf("%s (%d failures)\n",
                failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
