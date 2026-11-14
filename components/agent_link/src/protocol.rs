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
pub const SCREENSHOT: u8 = 0x40;
pub const SCREENSHOT_RENDER: u8 = 0x41;
pub const SCREENSHOT_SAVE: u8 = 0x42;
pub const SCREENSHOT_RELEASE: u8 = 0x43;
pub const RENDER_FILE: u8 = 0x44;
pub const REPLY: u8 = 0x80;

pub const TOUCH: u8 = 0xC0;
pub const KEY: u8 = 0xC1;
pub const INPUT_CLEAR: u8 = 0xC2;
pub const KEEPALIVE: u8 = 0xC3;

pub const MEDIA: u8 = 0xB0;

pub const VIDEO_START: u8 = 0x01;
pub const FRAME_ACK: u8 = 0xC0;
pub const FRAME: u8 = 0x81;

pub const AUDIO_START: u8 = 0x01;
pub const AUDIO: u8 = 0x81;
pub const VOLUME: u8 = 0x82;

pub const CAP_VIDEO: u32 = 1 << 0;
pub const CAP_AUDIO: u32 = 1 << 1;
pub const CAP_INPUT: u32 = 1 << 2;
pub const CAP_MEDIA: u32 = 1 << 3;
pub const CAP_PREVIEW: u32 = 1 << 4;
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
    /// Lowers quality, then frame rate, to what the link carries.
    pub adapt: bool,
    /// Bytes/s the video keeps under by lowering quality; 0 for no limit.
    pub max_rate: u32,
}

impl VideoStart {
    pub fn encode(&self) -> [u8; 12] {
        let [w0, w1] = self.box_width.to_le_bytes();
        let [h0, h1] = self.box_height.to_le_bytes();
        let [r0, r1, r2, r3] = self.max_rate.to_le_bytes();
        let flags = u8::from(self.swap) | u8::from(self.fill) << 1 | u8::from(self.adapt) << 2;
        [w0, w1, h0, h1, flags, self.quality, self.max_fps, self.strips, r0, r1, r2, r3]
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

pub fn is_notice(kind: u8) -> bool {
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

/// MEDIA_STATE: what GET_MEDIA answers and a MEDIA notice carries.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct MediaState {
    pub state: PlayState,
    pub has_art: bool,
    pub can_seek: bool,
    /// The device's media volume is muted.
    pub muted: bool,
    /// Changes with the track; 0 without a session.
    pub token: u32,
    pub position_ms: u32,
    /// 0 when unknown.
    pub duration_ms: u32,
    /// 1000 for normal speed, 0 when not playing.
    pub speed: u16,
    /// The device's media volume step, of `volume_max`.
    pub volume: u8,
    pub volume_max: u8,
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
            muted: b[1] & 4 != 0,
            token: u32_at(4),
            position_ms: u32_at(8),
            duration_ms: u32_at(12),
            speed: u16::from_le_bytes([b[16], b[17]]),
            volume: b[18],
            volume_max: b[19],
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
    /// Baseline JPEG; empty without art.
    pub art: &'a [u8],
}

impl<'a> MediaRender<'a> {
    pub fn parse(b: &'a [u8]) -> Option<Self> {
        let (title, rest) = TextImage::parse(b)?;
        let (subtitle, rest) = TextImage::parse(rest)?;
        let head = rest.get(..8)?;
        let (art_width, art_height) = (u16::from_le_bytes([head[0], head[1]]), u16::from_le_bytes([head[2], head[3]]));
        let length = u32::from_le_bytes([head[4], head[5], head[6], head[7]]) as usize;
        let art = rest.get(8..8usize.checked_add(length)?)?;
        Some(Self { title, subtitle, art_width, art_height, art })
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MediaAction {
    PlayPause,
    Next,
    Previous,
    Seek(u32),
    /// A step of the device's media volume.
    Volume(u8),
    Mute(bool),
}

pub fn media_control(action: MediaAction) -> [u8; 8] {
    let (code, position) = match action {
        MediaAction::PlayPause => (0, 0),
        MediaAction::Next => (1, 0),
        MediaAction::Previous => (2, 0),
        MediaAction::Seek(ms) => (3, ms),
        MediaAction::Volume(step) => (4, u32::from(step)),
        MediaAction::Mute(mute) => (5, u32::from(mute)),
    };
    let [p0, p1, p2, p3] = position.to_le_bytes();
    [code, 0, 0, 0, p0, p1, p2, p3]
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum AudioCodec {
    /// Interleaved S16LE.
    Pcm = 1,
    Opus = 2,
    /// Raw AAC-LC access units, without ADTS headers.
    Aac = 3,
}

/// What an audio stream takes from the device.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum AudioSource {
    Media = 0,
    /// Every playback but calls.
    All = 1,
}

pub const VOLUME_SCALE: u16 = 1000;

/// What the device plays at while an audio stream is open.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct AudioVolume {
    /// Of `VOLUME_SCALE`, the device's maximum.
    pub level: u16,
    pub muted: bool,
}

pub fn audio_start(codec: AudioCodec, source: AudioSource, volume: AudioVolume) -> [u8; 8] {
    let [l0, l1] = volume.level.to_le_bytes();
    [codec as u8, source as u8, l0, l1, u8::from(volume.muted), 0, 0, 0]
}

/// What the reply to AUDIO_START says the stream carries.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct AudioFormat {
    pub codec: AudioCodec,
    pub channels: u8,
    pub rate: u32,
}

impl AudioFormat {
    /// From a REPLY result.
    pub fn parse(b: &[u8]) -> Option<Self> {
        let b = b.get(..8)?;
        let codec = match b[0] {
            1 => AudioCodec::Pcm,
            2 => AudioCodec::Opus,
            3 => AudioCodec::Aac,
            _ => return None,
        };
        Some(Self { codec, channels: b[1], rate: u32::from_le_bytes([b[4], b[5], b[6], b[7]]) })
    }
}

pub const RENDER_IMAGE: u8 = 1 << 0;
pub const RENDER_ATTRS: u8 = 1 << 1;

pub const ATTR_WIDTH: u16 = 0x0001;
pub const ATTR_HEIGHT: u16 = 0x0002;
pub const ATTR_MIME: u16 = 0x0003;
pub const ATTR_EXIF_MAKE: u16 = 0x0100;
pub const ATTR_EXIF_MODEL: u16 = 0x0101;
pub const ATTR_EXIF_LENS: u16 = 0x0102;
pub const ATTR_EXIF_SOFTWARE: u16 = 0x0103;
pub const ATTR_EXIF_TAKEN: u16 = 0x0104;
pub const ATTR_EXIF_ISO: u16 = 0x0105;
pub const ATTR_EXIF_EXPOSURE: u16 = 0x0106;
pub const ATTR_EXIF_F_NUMBER: u16 = 0x0107;
pub const ATTR_EXIF_FOCAL: u16 = 0x0108;
pub const ATTR_EXIF_FOCAL_35MM: u16 = 0x0109;
pub const ATTR_EXIF_BIAS: u16 = 0x010A;
pub const ATTR_EXIF_FLASH: u16 = 0x010B;

/// SCREENSHOT and SCREENSHOT_RENDER.
pub fn screenshot(box_width: u16, box_height: u16, quality: u8) -> [u8; 8] {
    let [w0, w1] = box_width.to_le_bytes();
    let [h0, h1] = box_height.to_le_bytes();
    [w0, w1, h0, h1, quality, 0, 0, 0]
}

/// `flags` takes `RENDER_*`.
pub fn render_file(box_width: u16, box_height: u16, quality: u8, flags: u8, path: &str) -> Vec<u8> {
    let mut out = Vec::with_capacity(8 + path.len());
    out.extend_from_slice(&box_width.to_le_bytes());
    out.extend_from_slice(&box_height.to_le_bytes());
    out.extend_from_slice(&[quality, flags, 0, 0]);
    out.extend_from_slice(path.as_bytes());
    out
}

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum AttrValue<'a> {
    U32(u32),
    I32(i32),
    Ratio(u32, u32),
    SignedRatio(i32, i32),
    Text(&'a str),
}

#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Attr<'a> {
    pub key: u16,
    pub value: AttrValue<'a>,
}

/// A RENDERED result: a JPEG and attributes.
#[derive(Clone, PartialEq, Debug)]
pub struct Rendered<'a> {
    /// 0 without an image.
    pub width: u16,
    pub height: u16,
    pub jpeg: &'a [u8],
    /// Those of a type this end knows; the rest are skipped.
    pub attrs: Vec<Attr<'a>>,
}

impl<'a> Rendered<'a> {
    pub fn parse(b: &'a [u8]) -> Option<Self> {
        let head = b.get(..8)?;
        let width = u16::from_le_bytes([head[0], head[1]]);
        let height = u16::from_le_bytes([head[2], head[3]]);
        let length = u32::from_le_bytes([head[4], head[5], head[6], head[7]]) as usize;
        let jpeg = b.get(8..8usize.checked_add(length)?)?;
        let mut rest = &b[8 + length..];
        let count = u16::from_le_bytes([*rest.first()?, *rest.get(1)?]);
        rest = &rest[2..];
        let mut attrs = Vec::new();
        for _ in 0..count {
            let head = rest.get(..6)?;
            let key = u16::from_le_bytes([head[0], head[1]]);
            let length = usize::from(u16::from_le_bytes([head[4], head[5]]));
            let value = rest.get(6..6 + length)?;
            rest = &rest[6 + length..];
            let word = |at: usize| value.get(at..at + 4).map(|v| [v[0], v[1], v[2], v[3]]);
            let value = match head[2] {
                0 => AttrValue::U32(u32::from_le_bytes(word(0)?)),
                1 => AttrValue::I32(i32::from_le_bytes(word(0)?)),
                2 => AttrValue::Ratio(u32::from_le_bytes(word(0)?), u32::from_le_bytes(word(4)?)),
                3 => AttrValue::SignedRatio(i32::from_le_bytes(word(0)?), i32::from_le_bytes(word(4)?)),
                4 => AttrValue::Text(core::str::from_utf8(value).ok()?),
                _ => continue,
            };
            attrs.push(Attr { key, value });
        }
        Some(Self { width, height, jpeg, attrs })
    }

    pub fn attr(&self, key: u16) -> Option<AttrValue<'a>> {
        self.attrs.iter().find(|a| a.key == key).map(|a| a.value)
    }
}

/// What SCREENSHOT_SAVE answered.
pub fn saved(b: &[u8]) -> Option<(u64, String)> {
    let size = u32::from_le_bytes(b.get(..4)?.try_into().ok()?);
    Some((u64::from(size), String::from(core::str::from_utf8(&b[4..]).ok()?)))
}

