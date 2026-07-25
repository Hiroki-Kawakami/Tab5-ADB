#include "adb_spake2.hpp"

#include <psa/crypto.h>

#include <array>
#include <cstdio>
#include <cstring>

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
        random[i] = static_cast<uint8_t>(seed + i * 29);
    }
    return random;
}

bool run_exchange(const char* alice_password, const char* bob_password,
                  std::array<uint8_t, 64>& alice_key,
                  std::array<uint8_t, 64>& bob_key) {
    auto alice_random = make_random(7);
    auto bob_random = make_random(113);
    adb::pairing::Spake2 alice(adb::pairing::Spake2Role::Alice);
    adb::pairing::Spake2 bob(adb::pairing::Spake2Role::Bob);
    if (!alice.init(reinterpret_cast<const uint8_t*>(alice_password),
                    std::strlen(alice_password), alice_random.data()) ||
        !bob.init(reinterpret_cast<const uint8_t*>(bob_password),
                  std::strlen(bob_password), bob_random.data())) {
        return false;
    }
    return alice.process_message(bob.message().data(), bob.message().size(),
                                 alice_key) &&
           bob.process_message(alice.message().data(), alice.message().size(),
                               bob_key);
}

}  // namespace

int main() {
    std::printf("adb_spake2 self-test\n");
    CHECK(psa_crypto_init() == PSA_SUCCESS, "psa_crypto_init");

    std::array<uint8_t, 64> alice_key{};
    std::array<uint8_t, 64> bob_key{};
    CHECK(run_exchange("123456exporter-material", "123456exporter-material",
                       alice_key, bob_key),
          "Alice and Bob complete exchange");
    CHECK(alice_key == bob_key, "matching passwords derive the same key");

    alice_key.fill(0);
    bob_key.fill(0);
    CHECK(run_exchange("123456exporter-material", "654321exporter-material",
                       alice_key, bob_key),
          "wrong password still completes SPAKE2 math");
    CHECK(alice_key != bob_key, "wrong passwords derive different keys");

    auto random = make_random(41);
    adb::pairing::Spake2 invalid(adb::pairing::Spake2Role::Alice);
    CHECK(invalid.init(reinterpret_cast<const uint8_t*>("123456"), 6,
                       random.data()),
          "initialize for invalid peer test");
    std::array<uint8_t, 31> short_message{};
    std::array<uint8_t, 64> key{};
    CHECK(!invalid.process_message(short_message.data(), short_message.size(),
                                   key),
          "reject wrong message size");

    adb::pairing::Spake2 noncanonical(adb::pairing::Spake2Role::Alice);
    CHECK(noncanonical.init(reinterpret_cast<const uint8_t*>("123456"), 6,
                            random.data()),
          "initialize for point validation");
    std::array<uint8_t, 32> invalid_point{};
    invalid_point.fill(0xff);
    CHECK(!noncanonical.process_message(invalid_point.data(),
                                        invalid_point.size(), key),
          "reject non-canonical point");

    std::printf("%s (%d failures)\n",
                failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
