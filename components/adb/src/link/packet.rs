/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::vec::Vec;

pub(crate) const A_CNXN: u32 = 0x4e58_4e43;
pub(crate) const A_AUTH: u32 = 0x4854_5541;
pub(crate) const A_OPEN: u32 = 0x4e45_504f;
pub(crate) const A_OKAY: u32 = 0x5941_4b4f;
pub(crate) const A_CLSE: u32 = 0x4553_4c43;
pub(crate) const A_WRTE: u32 = 0x4554_5257;

pub(crate) const A_VERSION: u32 = 0x0100_0001;

pub(crate) const AUTH_TOKEN: u32 = 1;
pub(crate) const AUTH_SIGNATURE: u32 = 2;
pub(crate) const AUTH_RSAPUBLICKEY: u32 = 3;

pub(crate) const HEADER_BYTES: usize = 24;

#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Packet {
    pub command: u32,
    pub arg0: u32,
    pub arg1: u32,
    pub payload: Vec<u8>,
}

/// What a packet header says about the payload that follows it.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Header {
    pub command: u32,
    pub arg0: u32,
    pub arg1: u32,
    pub length: u32,
}

impl Packet {
    pub fn new(command: u32, arg0: u32, arg1: u32, payload: Vec<u8>) -> Self {
        Self { command, arg0, arg1, payload }
    }

    /// The checksum is still sent, for devices older than A_VERSION 0x01000001.
    pub fn header(&self) -> [u8; HEADER_BYTES] {
        let sum = self.payload.iter().fold(0u32, |s, &b| s.wrapping_add(u32::from(b)));
        let mut out = [0u8; HEADER_BYTES];
        for (i, word) in
            [self.command, self.arg0, self.arg1, self.payload.len() as u32, sum, !self.command].into_iter().enumerate()
        {
            out[i * 4..i * 4 + 4].copy_from_slice(&word.to_le_bytes());
        }
        out
    }
}

impl Header {
    /// `None` when the magic does not match the command.
    pub fn parse(b: &[u8; HEADER_BYTES]) -> Option<Self> {
        let word = |i: usize| u32::from_le_bytes([b[i * 4], b[i * 4 + 1], b[i * 4 + 2], b[i * 4 + 3]]);
        let command = word(0);
        (word(5) == !command).then(|| Self { command, arg0: word(1), arg1: word(2), length: word(3) })
    }
}
