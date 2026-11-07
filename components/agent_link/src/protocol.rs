/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

pub const PROTO_VERSION: u16 = 1;
pub const HEADER_BYTES: usize = 8;

pub const HELLO: u8 = 0x01;
pub const REPLY: u8 = 0x40;

pub const OK: u8 = 0x00;
pub const EINVAL: u8 = 0x01;
pub const ENOTSUP: u8 = 0x02;
pub const EBUSY: u8 = 0x03;
pub const ESTATE: u8 = 0x04;
pub const EFAIL: u8 = 0xff;

/// The status and the bytes after it, before a REPLY's result.
pub const REPLY_HEAD: usize = 4;
const HELLO_BYTES: usize = 20;
const HELLO_REPLY_BYTES: usize = 16;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Channel {
    Control,
    Video,
    Audio,
}

impl Channel {
    /// The `localabstract:` service of the agent started with `id`.
    pub fn service(self, id: &str) -> String {
        match self {
            Channel::Control => format!("localabstract:tab5adb-agent-{id}"),
            Channel::Video => format!("localabstract:tab5adb-agent-{id}-video"),
            Channel::Audio => format!("localabstract:tab5adb-agent-{id}-audio"),
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Header {
    pub length: u32,
    pub kind: u8,
    pub id: u16,
}

impl Header {
    pub fn parse(b: &[u8; HEADER_BYTES]) -> Self {
        Self {
            length: u32::from_le_bytes([b[0], b[1], b[2], b[3]]),
            kind: b[4],
            id: u16::from_le_bytes([b[6], b[7]]),
        }
    }

    pub fn write(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(&self.length.to_le_bytes());
        out.push(self.kind);
        out.push(0);
        out.extend_from_slice(&self.id.to_le_bytes());
    }
}

/// Appends one message to `out`.
pub fn message(out: &mut Vec<u8>, kind: u8, id: u16, payload: &[u8]) {
    Header { length: payload.len() as u32, kind, id }.write(out);
    out.extend_from_slice(payload);
}

/// The status of a REPLY payload and its result.
pub fn reply(payload: &[u8]) -> Option<(u8, &[u8])> {
    (payload.len() >= REPLY_HEAD).then(|| (payload[0], &payload[REPLY_HEAD..]))
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Hello {
    pub proto_version: u16,
    pub capabilities: u32,
    /// Control, video, audio.
    pub max_message: [u32; 3],
}

impl Hello {
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(HELLO_BYTES);
        out.extend_from_slice(&self.proto_version.to_le_bytes());
        out.extend_from_slice(&[0, 0]);
        out.extend_from_slice(&self.capabilities.to_le_bytes());
        for max in self.max_message {
            out.extend_from_slice(&max.to_le_bytes());
        }
        out
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct HelloReply {
    pub proto_version: u16,
    pub version: [u8; 3],
    pub capabilities: u32,
    pub max_message: u32,
}

impl HelloReply {
    /// Ignores what a later version appends.
    pub fn parse(b: &[u8]) -> Option<Self> {
        if b.len() < HELLO_REPLY_BYTES {
            return None;
        }
        Some(Self {
            proto_version: u16::from_le_bytes([b[0], b[1]]),
            version: [b[2], b[3], b[4]],
            capabilities: u32::from_le_bytes([b[8], b[9], b[10], b[11]]),
            max_message: u32::from_le_bytes([b[12], b[13], b[14], b[15]]),
        })
    }
}
