/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * BoringSSL's SPAKE2 over edwards25519, byte for byte, as adb pairing runs it.
 */

use devkit_crypto::ed25519::{Point, Scalar};
use devkit_crypto::hash::{Hash, Sha512};

pub(crate) const CLIENT_NAME: &[u8] = b"adb pair client\0";
pub(crate) const SERVER_NAME: &[u8] = b"adb pair server\0";

const M: [u8; 32] = [
    0x5a, 0xda, 0x7e, 0x4b, 0xf6, 0xdd, 0xd9, 0xad, 0xb6, 0x62, 0x6d, 0x32, 0x13, 0x1c, 0x6b, 0x5c, 0x51, 0xa1, 0xe3, 0x47, 0xa3, 0x47, 0x8f,
    0x53, 0xcf, 0xcf, 0x44, 0x1b, 0x88, 0xee, 0xd1, 0x2e,
];
const N: [u8; 32] = [
    0x10, 0xe3, 0xdf, 0x0a, 0xe3, 0x7d, 0x8e, 0x7a, 0x99, 0xb5, 0xfe, 0x74, 0xb4, 0x46, 0x72, 0x10, 0x3d, 0xbd, 0xdc, 0xbd, 0x06, 0xaf, 0x68,
    0x0d, 0x71, 0x32, 0x9a, 0x11, 0x69, 0x3b, 0xc7, 0x78,
];
/// ℓ, little-endian.
const ORDER: [u8; 32] = [
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0x10,
];

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Role {
    /// The pairing client, masking with M.
    Alice,
    #[cfg_attr(not(test), expect(dead_code))]
    Bob,
}

pub(crate) struct Spake2 {
    role: Role,
    private_key: [u8; 32],
    password_scalar: [u8; 32],
    password_hash: [u8; 64],
    message: [u8; 32],
}

impl Spake2 {
    /// `random` is the 64 bytes BoringSSL draws for the private key.
    pub(crate) fn new(role: Role, password: &[u8], random: &[u8; 64]) -> Self {
        let private_key = times_eight(Scalar::from_bytes_mod_order_wide(random).to_bytes());
        let password_hash: [u8; 64] = Sha512::digest(password).as_ref().try_into().unwrap();
        let password_scalar = clear_low_bits(Scalar::from_bytes_mod_order_wide(&password_hash).to_bytes());
        let mask = point(if role == Role::Alice { &M } else { &N }).mul_bits(&password_scalar);
        let message = Point::base().mul_bits(&private_key).add(&mask).encode();
        Self { role, private_key, password_scalar, password_hash, message }
    }

    pub(crate) fn message(&self) -> &[u8; 32] {
        &self.message
    }

    /// The 64-byte shared key; `None` when `theirs` is not a point.
    pub(crate) fn finish(&self, theirs: &[u8; 32]) -> Option<[u8; 64]> {
        let their_mask = point(if self.role == Role::Alice { &N } else { &M }).mul_bits(&self.password_scalar);
        let shared = Point::decode(theirs)?.sub(&their_mask).mul_bits(&self.private_key).encode();
        let (first, second, first_message, second_message) = match self.role {
            Role::Alice => (CLIENT_NAME, SERVER_NAME, &self.message, theirs),
            Role::Bob => (CLIENT_NAME, SERVER_NAME, theirs, &self.message),
        };
        let mut sha = Sha512::new();
        for part in [first, second, &first_message[..], &second_message[..], &shared[..], &self.password_hash[..]] {
            sha.update(&(part.len() as u64).to_le_bytes());
            sha.update(part);
        }
        Some(sha.finalize().as_ref().try_into().unwrap())
    }
}

fn point(bytes: &[u8; 32]) -> Point {
    Point::decode(bytes).unwrap()
}

fn times_eight(scalar: [u8; 32]) -> [u8; 32] {
    let mut out = [0u8; 32];
    let mut carry = 0;
    for (o, b) in out.iter_mut().zip(scalar) {
        *o = b << 3 | carry;
        carry = b >> 5;
    }
    out
}

/// Adds ℓ, 2ℓ and 4ℓ as the low bits ask, keeping the value modulo ℓ but a
/// multiple of eight: BoringSSL's fix for the password scalar it does not
/// multiply by the cofactor.
fn clear_low_bits(mut scalar: [u8; 32]) -> [u8; 32] {
    for bit in 0..3 {
        if scalar[0] >> bit & 1 == 1 {
            let mut carry = 0u16;
            let mut shifted = 0u16;
            for (s, &l) in scalar.iter_mut().zip(&ORDER) {
                let l = u16::from(l) << bit | shifted;
                shifted = l >> 8;
                let sum = u16::from(*s) + (l & 0xff) + carry;
                *s = sum as u8;
                carry = sum >> 8;
            }
        }
    }
    scalar
}
