#include "adb_spake2.hpp"

#include <mbedtls/platform_util.h>
#include <psa/crypto.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace adb {
namespace pairing {

namespace {

using Field = std::array<int64_t, 16>;
using Point = std::array<Field, 4>;
using Scalar = std::array<uint8_t, 32>;

constexpr Field kZero{};
constexpr Field kOne{1};
constexpr Field kD{
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203,
};
constexpr Field kD2{
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406,
};
constexpr Field kBaseX{
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169,
};
constexpr Field kBaseY{
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
};
constexpr Field kSqrtMinusOne{
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83,
};
constexpr Scalar kGroupOrder{
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};
// Encoded masking points fixed by BoringSSL's Ed25519 SPAKE2 profile.
constexpr std::array<uint8_t, 32> kSpakeM{
    0x5a, 0xda, 0x7e, 0x4b, 0xf6, 0xdd, 0xd9, 0xad,
    0xb6, 0x62, 0x6d, 0x32, 0x13, 0x1c, 0x6b, 0x5c,
    0x51, 0xa1, 0xe3, 0x47, 0xa3, 0x47, 0x8f, 0x53,
    0xcf, 0xcf, 0x44, 0x1b, 0x88, 0xee, 0xd1, 0x2e,
};
constexpr std::array<uint8_t, 32> kSpakeN{
    0x10, 0xe3, 0xdf, 0x0a, 0xe3, 0x7d, 0x8e, 0x7a,
    0x99, 0xb5, 0xfe, 0x74, 0xb4, 0x46, 0x72, 0x10,
    0x3d, 0xbd, 0xdc, 0xbd, 0x06, 0xaf, 0x68, 0x0d,
    0x71, 0x32, 0x9a, 0x11, 0x69, 0x3b, 0xc7, 0x78,
};

void carry(Field& value) {
    for (size_t i = 0; i < value.size(); ++i) {
        value[i] += 1LL << 16;
        int64_t c = value[i] >> 16;
        size_t next = i == 15 ? 0 : i + 1;
        value[next] += c - 1 + (i == 15 ? 37 * (c - 1) : 0);
        value[i] -= c * (1LL << 16);
    }
}

void select(Field& lhs, Field& rhs, uint8_t choose_rhs) {
    int64_t mask = -static_cast<int64_t>(choose_rhs);
    for (size_t i = 0; i < lhs.size(); ++i) {
        int64_t swap = mask & (lhs[i] ^ rhs[i]);
        lhs[i] ^= swap;
        rhs[i] ^= swap;
    }
}

void pack_field(uint8_t out[32], const Field& input) {
    Field value = input;
    Field reduced{};
    carry(value);
    carry(value);
    carry(value);
    for (int pass = 0; pass < 2; ++pass) {
        reduced[0] = value[0] - 0xffed;
        for (size_t i = 1; i < 15; ++i) {
            reduced[i] = value[i] - 0xffff -
                         ((reduced[i - 1] >> 16) & 1);
            reduced[i - 1] &= 0xffff;
        }
        reduced[15] = value[15] - 0x7fff -
                      ((reduced[14] >> 16) & 1);
        uint8_t borrow = static_cast<uint8_t>((reduced[15] >> 16) & 1);
        reduced[14] &= 0xffff;
        select(value, reduced, static_cast<uint8_t>(1 - borrow));
    }
    for (size_t i = 0; i < 16; ++i) {
        out[2 * i] = static_cast<uint8_t>(value[i]);
        out[2 * i + 1] = static_cast<uint8_t>(value[i] >> 8);
    }
}

void unpack_field(Field& out, const uint8_t input[32]) {
    for (size_t i = 0; i < 16; ++i) {
        out[i] = static_cast<int64_t>(input[2 * i]) |
                 (static_cast<int64_t>(input[2 * i + 1]) << 8);
    }
    out[15] &= 0x7fff;
}

void add_field(Field& out, const Field& lhs, const Field& rhs) {
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = lhs[i] + rhs[i];
    }
}

void subtract_field(Field& out, const Field& lhs, const Field& rhs) {
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = lhs[i] - rhs[i];
    }
}

void multiply_field(Field& out, const Field& lhs, const Field& rhs) {
    std::array<int64_t, 31> product{};
    for (size_t i = 0; i < 16; ++i) {
        for (size_t j = 0; j < 16; ++j) {
            product[i + j] += lhs[i] * rhs[j];
        }
    }
    for (size_t i = 0; i < 15; ++i) {
        product[i] += 38 * product[i + 16];
    }
    std::copy_n(product.begin(), 16, out.begin());
    carry(out);
    carry(out);
}

void square_field(Field& out, const Field& input) {
    multiply_field(out, input, input);
}

void inverse_field(Field& out, const Field& input) {
    Field value = input;
    for (int i = 253; i >= 0; --i) {
        square_field(value, value);
        if (i != 2 && i != 4) {
            multiply_field(value, value, input);
        }
    }
    out = value;
}

void pow2523(Field& out, const Field& input) {
    Field value = input;
    for (int i = 250; i >= 0; --i) {
        square_field(value, value);
        if (i != 1) {
            multiply_field(value, value, input);
        }
    }
    out = value;
}

bool fields_equal(const Field& lhs, const Field& rhs) {
    std::array<uint8_t, 32> lhs_bytes{};
    std::array<uint8_t, 32> rhs_bytes{};
    pack_field(lhs_bytes.data(), lhs);
    pack_field(rhs_bytes.data(), rhs);
    uint8_t diff = 0;
    for (size_t i = 0; i < lhs_bytes.size(); ++i) {
        diff |= lhs_bytes[i] ^ rhs_bytes[i];
    }
    return diff == 0;
}

uint8_t field_parity(const Field& value) {
    std::array<uint8_t, 32> encoded{};
    pack_field(encoded.data(), value);
    return encoded[0] & 1;
}

void point_add(Point& lhs, const Point& rhs) {
    Field a{}, b{}, c{}, d{}, e{}, f{}, g{}, h{}, t{};
    subtract_field(a, lhs[1], lhs[0]);
    subtract_field(t, rhs[1], rhs[0]);
    multiply_field(a, a, t);
    add_field(b, lhs[0], lhs[1]);
    add_field(t, rhs[0], rhs[1]);
    multiply_field(b, b, t);
    multiply_field(c, lhs[3], rhs[3]);
    multiply_field(c, c, kD2);
    multiply_field(d, lhs[2], rhs[2]);
    add_field(d, d, d);
    subtract_field(e, b, a);
    subtract_field(f, d, c);
    add_field(g, d, c);
    add_field(h, b, a);
    multiply_field(lhs[0], e, f);
    multiply_field(lhs[1], h, g);
    multiply_field(lhs[2], g, f);
    multiply_field(lhs[3], e, h);
}

void point_select(Point& lhs, Point& rhs, uint8_t choose_rhs) {
    for (size_t i = 0; i < lhs.size(); ++i) {
        select(lhs[i], rhs[i], choose_rhs);
    }
}

void point_multiply(Point& out, const Point& input,
                    const Scalar& scalar) {
    Point result{kZero, kOne, kOne, kZero};
    Point addend = input;
    for (int i = 255; i >= 0; --i) {
        uint8_t bit = (scalar[static_cast<size_t>(i) >> 3] >>
                       (i & 7)) & 1;
        point_select(result, addend, bit);
        point_add(addend, result);
        point_add(result, result);
        point_select(result, addend, bit);
    }
    out = result;
}

void point_multiply_base(Point& out, const Scalar& scalar) {
    Point base{kBaseX, kBaseY, kOne, kZero};
    multiply_field(base[3], base[0], base[1]);
    point_multiply(out, base, scalar);
}

void point_negate(Point& point) {
    subtract_field(point[0], kZero, point[0]);
    subtract_field(point[3], kZero, point[3]);
}

void point_encode(uint8_t out[32], const Point& point) {
    Field reciprocal{}, x{}, y{};
    inverse_field(reciprocal, point[2]);
    multiply_field(x, point[0], reciprocal);
    multiply_field(y, point[1], reciprocal);
    pack_field(out, y);
    out[31] ^= static_cast<uint8_t>(field_parity(x) << 7);
}

bool point_decode(Point& out, const uint8_t input[32]) {
    std::array<uint8_t, 32> canonical_input{};
    std::copy_n(input, canonical_input.size(), canonical_input.begin());
    uint8_t sign = canonical_input[31] >> 7;
    canonical_input[31] &= 0x7f;

    Field y{};
    unpack_field(y, input);
    std::array<uint8_t, 32> canonical_y{};
    pack_field(canonical_y.data(), y);
    if (canonical_y != canonical_input) {
        return false;
    }

    Field numerator{}, denominator{}, den2{}, den4{}, den6{}, candidate{}, check{};
    square_field(numerator, y);
    multiply_field(denominator, numerator, kD);
    subtract_field(numerator, numerator, kOne);
    add_field(denominator, denominator, kOne);

    square_field(den2, denominator);
    square_field(den4, den2);
    multiply_field(den6, den4, den2);
    multiply_field(candidate, den6, numerator);
    multiply_field(candidate, candidate, denominator);
    pow2523(candidate, candidate);
    multiply_field(candidate, candidate, numerator);
    multiply_field(candidate, candidate, denominator);
    multiply_field(candidate, candidate, denominator);
    multiply_field(candidate, candidate, denominator);

    square_field(check, candidate);
    multiply_field(check, check, denominator);
    if (!fields_equal(check, numerator)) {
        multiply_field(candidate, candidate, kSqrtMinusOne);
        square_field(check, candidate);
        multiply_field(check, check, denominator);
        if (!fields_equal(check, numerator)) {
            return false;
        }
    }

    if (field_parity(candidate) != sign) {
        subtract_field(candidate, kZero, candidate);
    }
    if (sign != 0 && fields_equal(candidate, kZero)) {
        return false;
    }

    out[0] = candidate;
    out[1] = y;
    out[2] = kOne;
    multiply_field(out[3], out[0], out[1]);
    return true;
}

void reduce_scalar(Scalar& out, const uint8_t input[64]) {
    std::array<int64_t, 64> value{};
    for (size_t i = 0; i < value.size(); ++i) {
        value[i] = input[i];
    }
    for (int i = 63; i >= 32; --i) {
        int64_t carry_value = 0;
        int j = i - 32;
        for (; j < i - 12; ++j) {
            value[j] += carry_value -
                        16 * value[i] * kGroupOrder[j - (i - 32)];
            carry_value = (value[j] + 128) >> 8;
            value[j] -= carry_value * 256;
        }
        value[j] += carry_value;
        value[i] = 0;
    }

    int64_t carry_value = 0;
    for (size_t j = 0; j < 32; ++j) {
        value[j] += carry_value -
                    (value[31] >> 4) * kGroupOrder[j];
        carry_value = value[j] >> 8;
        value[j] &= 255;
    }
    for (size_t j = 0; j < 32; ++j) {
        value[j] -= carry_value * kGroupOrder[j];
    }
    for (size_t i = 0; i < 32; ++i) {
        value[i + 1] += value[i] >> 8;
        out[i] = static_cast<uint8_t>(value[i] & 255);
    }
}

void shift_left_three(Scalar& scalar) {
    uint8_t carry_value = 0;
    for (uint8_t& byte : scalar) {
        uint8_t next_carry = byte >> 5;
        byte = static_cast<uint8_t>((byte << 3) | carry_value);
        carry_value = next_carry;
    }
}

void add_shifted_order(Scalar& scalar, unsigned shift) {
    uint16_t carry_value = 0;
    for (size_t i = 0; i < scalar.size(); ++i) {
        uint16_t sum = static_cast<uint16_t>(scalar[i]) +
                       (static_cast<uint16_t>(kGroupOrder[i]) << shift) +
                       carry_value;
        scalar[i] = static_cast<uint8_t>(sum);
        carry_value = sum >> 8;
    }
}

void apply_password_scalar_compat(Scalar& scalar) {
    // Preserve the scalar modulo L while matching BoringSSL's divisible-by-8
    // representation.
    if ((scalar[0] & 1) != 0) {
        add_shifted_order(scalar, 0);
    }
    if ((scalar[0] & 2) != 0) {
        add_shifted_order(scalar, 1);
    }
    if ((scalar[0] & 4) != 0) {
        add_shifted_order(scalar, 2);
    }
}

bool hash_sha512(const uint8_t* input, size_t input_len,
                 std::array<uint8_t, 64>& output) {
    size_t output_len = 0;
    return psa_hash_compute(PSA_ALG_SHA_512, input, input_len,
                            output.data(), output.size(),
                            &output_len) == PSA_SUCCESS &&
           output_len == output.size();
}

bool hash_update_with_length(psa_hash_operation_t& operation,
                             const uint8_t* data, size_t len) {
    std::array<uint8_t, 8> length{};
    uint64_t value = len;
    for (uint8_t& byte : length) {
        byte = static_cast<uint8_t>(value);
        value >>= 8;
    }
    return psa_hash_update(&operation, length.data(), length.size()) ==
               PSA_SUCCESS &&
           psa_hash_update(&operation, data, len) == PSA_SUCCESS;
}

}  // namespace

Spake2::Spake2(Spake2Role role) : role_(role) {}

Spake2::~Spake2() {
    mbedtls_platform_zeroize(private_scalar_.data(), private_scalar_.size());
    mbedtls_platform_zeroize(password_scalar_.data(), password_scalar_.size());
    mbedtls_platform_zeroize(password_hash_.data(), password_hash_.size());
}

bool Spake2::init(const uint8_t* password, size_t password_len,
                  const uint8_t random[64]) {
    if (state_ != State::Empty || !password || password_len == 0 || !random ||
        psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }

    reduce_scalar(private_scalar_, random);
    shift_left_three(private_scalar_);
    if (!hash_sha512(password, password_len, password_hash_)) {
        return false;
    }
    reduce_scalar(password_scalar_, password_hash_.data());
    apply_password_scalar_compat(password_scalar_);

    Point ephemeral{};
    point_multiply_base(ephemeral, private_scalar_);

    const auto& mask_encoding =
        role_ == Spake2Role::Alice ? kSpakeM : kSpakeN;
    Point mask_base{};
    if (!point_decode(mask_base, mask_encoding.data())) {
        return false;
    }
    Point mask{};
    point_multiply(mask, mask_base, password_scalar_);
    point_add(ephemeral, mask);
    point_encode(message_.data(), ephemeral);
    state_ = State::MessageReady;
    return true;
}

bool Spake2::process_message(
    const uint8_t* peer_message, size_t peer_message_len,
    std::array<uint8_t, 64>& key_material) {
    if (state_ != State::MessageReady || !peer_message ||
        peer_message_len != 32) {
        return false;
    }
    state_ = State::Complete;

    Point peer{};
    if (!point_decode(peer, peer_message)) {
        return false;
    }
    const auto& mask_encoding =
        role_ == Spake2Role::Alice ? kSpakeN : kSpakeM;
    Point mask_base{};
    if (!point_decode(mask_base, mask_encoding.data())) {
        return false;
    }
    Point mask{};
    point_multiply(mask, mask_base, password_scalar_);
    point_negate(mask);
    point_add(peer, mask);

    Point shared{};
    point_multiply(shared, peer, private_scalar_);
    std::array<uint8_t, 32> encoded_shared{};
    point_encode(encoded_shared.data(), shared);

    // sizeof intentionally includes the protocol-required terminating NUL.
    static constexpr uint8_t kClientName[] = "adb pair client";
    static constexpr uint8_t kServerName[] = "adb pair server";

    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    bool ok = psa_hash_setup(&hash, PSA_ALG_SHA_512) == PSA_SUCCESS;
    const uint8_t* alice_message =
        role_ == Spake2Role::Alice ? message_.data() : peer_message;
    const uint8_t* bob_message =
        role_ == Spake2Role::Alice ? peer_message : message_.data();
    if (ok) {
        ok = hash_update_with_length(hash, kClientName,
                                     sizeof(kClientName)) &&
             hash_update_with_length(hash, kServerName,
                                     sizeof(kServerName)) &&
             hash_update_with_length(hash, alice_message, 32) &&
             hash_update_with_length(hash, bob_message, 32) &&
             hash_update_with_length(hash, encoded_shared.data(),
                                     encoded_shared.size()) &&
             hash_update_with_length(hash, password_hash_.data(),
                                     password_hash_.size());
    }
    size_t output_len = 0;
    if (ok) {
        ok = psa_hash_finish(&hash, key_material.data(),
                             key_material.size(), &output_len) == PSA_SUCCESS &&
             output_len == key_material.size();
    }
    if (!ok) {
        psa_hash_abort(&hash);
        key_material.fill(0);
    }
    mbedtls_platform_zeroize(encoded_shared.data(), encoded_shared.size());
    return ok;
}

}  // namespace pairing
}  // namespace adb
