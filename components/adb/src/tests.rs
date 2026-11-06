/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use crate::link::base64;
use crate::link::identity::{n0inv, r_squared};
use crate::link::packet::{A_CNXN, A_VERSION, Header, Packet};

#[test]
fn packet_header() {
    let p = Packet::new(A_CNXN, A_VERSION, 4096, b"host::\0".to_vec());
    let h = p.header();
    assert_eq!(&h[..4], b"CNXN");
    assert_eq!(&h[12..16], &7u32.to_le_bytes());
    let sum: u32 = b"host::\0".iter().map(|&b| u32::from(b)).sum();
    assert_eq!(&h[16..20], &sum.to_le_bytes());
    assert_eq!(Header::parse(&h), Some(Header { command: A_CNXN, arg0: A_VERSION, arg1: 4096, length: 7 }));
    let mut bad = h;
    bad[20] ^= 1;
    assert_eq!(Header::parse(&bad), None);
}

#[test]
fn base64_vectors() {
    for (input, output) in [("", ""), ("f", "Zg=="), ("fo", "Zm8="), ("foo", "Zm9v"), ("foobar", "Zm9vYmFy")] {
        assert_eq!(base64::encode(input.as_bytes()), output.as_bytes());
    }
}

#[test]
fn r_squared_matches_u128() {
    for n in [0xffff_ffff_ffff_ffc5u64, 0x8000_0000_0000_001d, 0x0123_4567_89ab_cdef] {
        let r = (1u128 << 64) % u128::from(n);
        let expected = (r * r) % u128::from(n);
        let words = r_squared(&[n as u32, (n >> 32) as u32]);
        assert_eq!(u128::from(words[0]) | u128::from(words[1]) << 32, expected);
    }
}

#[test]
fn n0inv_inverts() {
    for n0 in [1u32, 3, 0x1234_5679, 0xffff_ffff] {
        assert_eq!(n0.wrapping_mul(n0inv(n0)), u32::MAX);
    }
}
