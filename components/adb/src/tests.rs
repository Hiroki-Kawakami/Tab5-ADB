/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use crate::link::base64;
use crate::link::connection::banner_has_feature;
use crate::link::identity::{n0inv, r_squared};
use crate::link::packet::{A_CNXN, A_VERSION, Header, Packet};
use crate::services::shell::{packet, window_size};
use crate::services::sync::stat_v2;

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

#[test]
fn shell_protocol_packets() {
    assert_eq!(packet(0, b"ls\r"), [0, 3, 0, 0, 0, b'l', b's', b'\r']);
    assert_eq!(window_size(42, 72), b"42x72,0x0\0");
}

#[test]
fn banner_features() {
    let banner = "device::ro.product.model=Phone;features=shell_v2,cmd,stat_v2,ls_v2";
    assert!(banner_has_feature(banner, "stat_v2"));
    assert!(!banner_has_feature(banner, "stat"));
    assert!(!banner_has_feature("device::ro.product.model=Phone", "stat_v2"));
}

#[test]
fn stat_v2_reply() {
    let mut reply = [0u8; 72];
    reply[..4].copy_from_slice(b"STA2");
    reply[24..28].copy_from_slice(&0o040755u32.to_le_bytes());
    reply[40..48].copy_from_slice(&0x1_0000_0002u64.to_le_bytes());
    reply[56..64].copy_from_slice(&1_700_000_000i64.to_le_bytes());
    let stat = stat_v2(&reply).unwrap();
    assert!(stat.is_dir());
    assert_eq!((stat.size, stat.mtime), (0x1_0000_0002, 1_700_000_000));
    reply[4..8].copy_from_slice(&2u32.to_le_bytes());
    assert!(!stat_v2(&reply).unwrap().exists());
    reply[..4].copy_from_slice(b"STAT");
    assert!(stat_v2(&reply).is_err());
}
