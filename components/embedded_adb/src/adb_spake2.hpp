// ADB-specific implementation of BoringSSL's legacy Ed25519 SPAKE2 profile.
// Kept private because its transcript and scalar rules are not a generic SPAKE2
// API and exist only for Android pairing compatibility.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace adb {
namespace pairing {

enum class Spake2Role {
    Alice,  // pairing client
    Bob,    // Android pairing service
};

class Spake2 {
public:
    explicit Spake2(Spake2Role role);
    ~Spake2();

    Spake2(const Spake2&) = delete;
    Spake2& operator=(const Spake2&) = delete;

    // One-shot exchange: init emits the local message; process_message consumes
    // the peer message and returns the 64-byte transcript key material.
    bool init(const uint8_t* password, size_t password_len,
              const uint8_t random[64]);
    const std::array<uint8_t, 32>& message() const { return message_; }
    bool process_message(const uint8_t* peer_message, size_t peer_message_len,
                         std::array<uint8_t, 64>& key_material);

private:
    enum class State {
        Empty,
        MessageReady,
        Complete,
    };

    Spake2Role role_;
    State state_ = State::Empty;
    std::array<uint8_t, 32> private_scalar_{};
    std::array<uint8_t, 32> password_scalar_{};
    std::array<uint8_t, 64> password_hash_{};
    std::array<uint8_t, 32> message_{};
};

}  // namespace pairing
}  // namespace adb
