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
pub const ADAPT_DISPLAY: u8 = 0x11;
pub const GET_APP_INFO: u8 = 0x20;
pub const GET_MEDIA: u8 = 0x30;
pub const GET_MEDIA_RENDER: u8 = 0x31;
pub const MEDIA_CONTROL: u8 = 0x32;
pub const REPLY: u8 = 0x80;

pub const TOUCH: u8 = 0xC0;
pub const KEY: u8 = 0xC1;
pub const INPUT_CLEAR: u8 = 0xC2;

pub const MEDIA: u8 = 0xB0;

pub const VIDEO_START: u8 = 0x01;
pub const FRAME: u8 = 0x81;

pub const CAP_VIDEO: u32 = 1 << 0;
pub const CAP_INPUT: u32 = 1 << 2;
pub const CAP_MEDIA: u32 = 1 << 3;
pub const CAP_APPS: u32 = 1 << 5;

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
    /// Covers the box and sends its middle, instead of fitting in it.
    pub fill: bool,
}

impl VideoStart {
    pub fn encode(&self) -> [u8; 8] {
        let [w0, w1] = self.box_width.to_le_bytes();
        let [h0, h1] = self.box_height.to_le_bytes();
        [w0, w1, h0, h1, u8::from(self.swap) | u8::from(self.fill) << 1, self.quality, self.max_fps, self.strips]
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

pub fn adapt_display(width: u16, height: u16) -> [u8; 4] {
    let [w0, w1] = width.to_le_bytes();
    let [h0, h1] = height.to_le_bytes();
    [w0, w1, h0, h1]
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

/// One line of text the agent drew, as TEXT_IMAGE.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct TextImage<'a> {
    pub width: u16,
    /// The line's, whatever the text.
    pub height: u16,
    /// From the top to the baseline.
    pub ascent: u16,
    breaks: &'a [u8],
    /// `width` × `height`, no row padding.
    pub alpha: &'a [u8],
}

impl<'a> TextImage<'a> {
    /// The image and what follows it.
    pub fn parse(b: &'a [u8]) -> Option<(Self, &'a [u8])> {
        let u16_at = |i: usize| b.get(i..i + 2).map(|s| u16::from_le_bytes([s[0], s[1]]));
        let (width, height, ascent, count) = (u16_at(0)?, u16_at(2)?, u16_at(4)?, usize::from(u16_at(6)?));
        let breaks = b.get(8..8 + 2 * count)?;
        let at = 8 + 2 * count;
        let size = if width == 0 { 0 } else { usize::from(width) * usize::from(height) };
        let alpha = b.get(at..at + size)?;
        Some((Self { width, height, ascent, breaks, alpha }, &b[at + size..]))
    }

    /// The right edges of the characters, ascending.
    pub fn breaks(&self) -> impl Iterator<Item = u16> + 'a {
        self.breaks.chunks_exact(2).map(|s| u16::from_le_bytes([s[0], s[1]]))
    }
}

/// One package of a GET_APP_INFO reply.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct AppEntry<'a> {
    pub name: TextImage<'a>,
    pub icon_width: u16,
    pub icon_height: u16,
    /// B, G, R, A per pixel, not premultiplied; empty without an icon.
    pub icon: &'a [u8],
}

pub fn get_app_info(icon_px: u16, text_px: u8, packages: &[&str]) -> Option<Vec<u8>> {
    let count = u8::try_from(packages.len()).ok().filter(|&n| n > 0)?;
    let mut out = Vec::new();
    out.extend_from_slice(&icon_px.to_le_bytes());
    out.extend_from_slice(&[text_px, count]);
    for package in packages {
        out.push(u8::try_from(package.len()).ok()?);
        out.extend_from_slice(package.as_bytes());
    }
    Some(out)
}

/// `None` for each package the device does not have.
pub fn app_entries(b: &[u8], count: usize) -> Option<Vec<Option<AppEntry<'_>>>> {
    let mut entries = Vec::with_capacity(count);
    let mut rest = b;
    for _ in 0..count {
        let found = *rest.first()?;
        rest = rest.get(4..)?;
        if found == 0 {
            entries.push(None);
            continue;
        }
        let (name, after) = TextImage::parse(rest)?;
        let size = after.get(..4)?;
        let (icon_width, icon_height) = (u16::from_le_bytes([size[0], size[1]]), u16::from_le_bytes([size[2], size[3]]));
        let bytes = usize::from(icon_width) * usize::from(icon_height) * 4;
        let icon = after.get(4..4 + bytes)?;
        rest = &after[4 + bytes..];
        entries.push(Some(AppEntry { name, icon_width, icon_height, icon }));
    }
    Some(entries)
}

pub fn is_event(kind: u8) -> bool {
    (0x81..=0xBF).contains(&kind)
}

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum PlayState {
    #[default]
    None,
    Playing,
    Paused,
    Buffering,
    Stopped,
}

/// MEDIA_STATE: what GET_MEDIA answers and a MEDIA event carries.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct MediaState {
    pub state: PlayState,
    pub has_art: bool,
    pub can_seek: bool,
    /// Changes with the track; 0 without a session.
    pub token: u32,
    pub position_ms: u32,
    /// 0 when unknown.
    pub duration_ms: u32,
    /// 1000 for normal speed, 0 when not playing.
    pub speed: u16,
}

impl MediaState {
    pub fn parse(b: &[u8]) -> Option<Self> {
        let b = b.get(..20)?;
        let u32_at = |i: usize| u32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]]);
        let state = match b[0] {
            1 => PlayState::Playing,
            2 => PlayState::Paused,
            3 => PlayState::Buffering,
            4 => PlayState::Stopped,
            _ => PlayState::None,
        };
        Some(Self {
            state,
            has_art: b[1] & 1 != 0,
            can_seek: b[1] & 2 != 0,
            token: u32_at(4),
            position_ms: u32_at(8),
            duration_ms: u32_at(12),
            speed: u16::from_le_bytes([b[16], b[17]]),
        })
    }
}

pub const SUBTITLE_ARTIST: u8 = 1 << 0;
pub const SUBTITLE_ALBUM: u8 = 1 << 1;
pub const SUBTITLE_APP: u8 = 1 << 2;

/// `subtitle` takes `SUBTITLE_*`.
pub fn get_media_render(title_px: u8, subtitle_px: u8, art_px: u16, subtitle: u8) -> [u8; 8] {
    let [a0, a1] = art_px.to_le_bytes();
    [title_px, subtitle_px, a0, a1, subtitle, 0, 0, 0]
}

/// What GET_MEDIA_RENDER answered.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct MediaRender<'a> {
    pub title: TextImage<'a>,
    pub subtitle: TextImage<'a>,
    pub art_width: u16,
    pub art_height: u16,
    /// B, G, R, A per pixel, not premultiplied; empty without art.
    pub art: &'a [u8],
}

impl<'a> MediaRender<'a> {
    pub fn parse(b: &'a [u8]) -> Option<Self> {
        let (title, rest) = TextImage::parse(b)?;
        let (subtitle, rest) = TextImage::parse(rest)?;
        let size = rest.get(..4)?;
        let (art_width, art_height) = (u16::from_le_bytes([size[0], size[1]]), u16::from_le_bytes([size[2], size[3]]));
        let art = rest.get(4..4 + usize::from(art_width) * usize::from(art_height) * 4)?;
        Some(Self { title, subtitle, art_width, art_height, art })
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MediaAction {
    PlayPause,
    Next,
    Previous,
    Seek(u32),
}

pub fn media_control(action: MediaAction) -> [u8; 8] {
    let (code, position) = match action {
        MediaAction::PlayPause => (0, 0),
        MediaAction::Next => (1, 0),
        MediaAction::Previous => (2, 0),
        MediaAction::Seek(ms) => (3, ms),
    };
    let [p0, p1, p2, p3] = position.to_le_bytes();
    [code, 0, 0, 0, p0, p1, p2, p3]
}
