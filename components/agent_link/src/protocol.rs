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
pub const SET_ROTATION: u8 = 0x10;
pub const REPLY: u8 = 0x40;

pub const TOUCH: u8 = 0x60;
pub const KEY: u8 = 0x61;
pub const INPUT_CLEAR: u8 = 0x62;

pub const VIDEO_START: u8 = 0x01;
pub const FRAME: u8 = 0x41;

pub const CAP_VIDEO: u32 = 1 << 0;

pub const CODEC_JPEG: u8 = 1;
/// `SET_ROTATION` to give the device's own rotation back.
pub const ROTATION_RELEASE: u8 = 0xFF;

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
pub const FRAME_HEAD: usize = 12;
pub const MAX_TOUCHES: usize = 10;
const FLAG_LAST: u8 = 0x01;

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

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct VideoStart {
    pub box_width: u16,
    pub box_height: u16,
    /// The box turns with the device when it is on its side.
    pub swap: bool,
    /// 0 for the agent's default.
    pub quality: u8,
    /// 0 for no limit.
    pub max_fps: u8,
    /// Horizontal bands a frame comes in; 0 and 1 for whole frames.
    pub strips: u8,
}

impl VideoStart {
    pub fn encode(&self) -> [u8; 8] {
        let [w0, w1] = self.box_width.to_le_bytes();
        let [h0, h1] = self.box_height.to_le_bytes();
        [w0, w1, h0, h1, u8::from(self.swap), self.quality, self.max_fps, self.strips]
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct TouchPoint {
    pub id: u8,
    /// Pixels of the frame.
    pub x: u16,
    pub y: u16,
}

/// Every finger on the device's screen, against the frame it was placed on.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct Touch {
    pub time_ms: u32,
    pub width: u16,
    pub height: u16,
    pub rotation: u8,
    /// At most `MAX_TOUCHES`, each id once.
    pub points: Vec<TouchPoint>,
}

impl Touch {
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(12 + 6 * self.points.len());
        out.extend_from_slice(&self.time_ms.to_le_bytes());
        out.extend_from_slice(&self.width.to_le_bytes());
        out.extend_from_slice(&self.height.to_le_bytes());
        out.extend_from_slice(&[self.rotation, self.points.len() as u8, 0, 0]);
        for p in &self.points {
            out.extend_from_slice(&[p.id, 0]);
            out.extend_from_slice(&p.x.to_le_bytes());
            out.extend_from_slice(&p.y.to_le_bytes());
        }
        out
    }
}

pub fn key(keycode: u16, down: bool) -> [u8; 4] {
    let [k0, k1] = keycode.to_le_bytes();
    [k0, k1, if down { 0 } else { 1 }, 0]
}

/// One band of a frame.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct FrameHead {
    /// The whole frame.
    pub width: u16,
    pub height: u16,
    /// The band's first row and how many it has; its JPEG is `width` × `rows`.
    pub y: u16,
    pub rows: u16,
    /// `Surface.ROTATION_*` of the device when it was captured.
    pub rotation: u8,
    pub last: bool,
}

impl FrameHead {
    pub fn parse(b: &[u8; FRAME_HEAD]) -> Self {
        let u16_at = |i: usize| u16::from_le_bytes([b[i], b[i + 1]]);
        Self { width: u16_at(0), height: u16_at(2), y: u16_at(4), rows: u16_at(6), rotation: b[8] & 3, last: b[9] & FLAG_LAST != 0 }
    }
}
