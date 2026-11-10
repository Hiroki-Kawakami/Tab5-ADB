/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::String;
use alloc::vec::Vec;

const MAX_ENTRIES: usize = 512;
/// A text tag is longer than any field here only in a file that means nothing
/// good by it, and scanning all of one costs real time.
const MAX_ASCII: usize = 256;
const MAX_WEBP_CHUNKS: usize = 64;
const MAX_WEBP_EXIF: usize = 64 * 1024;

/// The tags a camera leaves in a picture. Text holds ASCII only.
#[derive(Clone, Default, PartialEq, Debug)]
pub struct Exif {
    pub make: String,
    pub model: String,
    pub lens: String,
    pub software: String,
    /// "YYYY:MM:DD HH:MM:SS" as stored.
    pub taken: String,
    pub iso: Option<u32>,
    /// Seconds, as a fraction.
    pub shutter: Option<(u32, u32)>,
    pub aperture: Option<f32>,
    pub focal_mm: Option<f32>,
    pub focal35_mm: Option<u16>,
    pub exposure_bias: Option<f32>,
    pub flash: Option<u16>,
    /// 1 to 8, the TIFF order; 0 when absent.
    pub orientation: u8,
}

impl Exif {
    pub fn is_empty(&self) -> bool {
        *self == Self::default()
    }
}

struct Tiff<'a> {
    data: &'a [u8],
    big_endian: bool,
}

struct Entry {
    tag: u16,
    kind: u16,
    at: usize,
    bytes: usize,
}

impl Tiff<'_> {
    fn has(&self, at: usize, bytes: usize) -> bool {
        at <= self.data.len() && self.data.len() - at >= bytes
    }

    fn u16(&self, at: usize) -> u16 {
        if !self.has(at, 2) {
            return 0;
        }
        let b = [self.data[at], self.data[at + 1]];
        if self.big_endian { u16::from_be_bytes(b) } else { u16::from_le_bytes(b) }
    }

    fn u32(&self, at: usize) -> u32 {
        if !self.has(at, 4) {
            return 0;
        }
        let b = [self.data[at], self.data[at + 1], self.data[at + 2], self.data[at + 3]];
        if self.big_endian { u32::from_be_bytes(b) } else { u32::from_le_bytes(b) }
    }

    fn entry(&self, at: usize) -> Option<Entry> {
        if !self.has(at, 12) {
            return None;
        }
        let kind = self.u16(at + 2);
        let count = self.u32(at + 4) as usize;
        let unit = match kind {
            1 | 2 | 6 | 7 => 1,
            3 | 8 => 2,
            4 | 9 | 11 => 4,
            5 | 10 | 12 => 8,
            _ => 0,
        };
        if unit == 0 || count == 0 || count > self.data.len() {
            return None;
        }
        let bytes = unit * count;
        let value = if bytes <= 4 { at + 8 } else { self.u32(at + 8) as usize };
        self.has(value, bytes).then_some(Entry { tag: self.u16(at), kind, at: value, bytes })
    }

    fn uint(&self, e: &Entry) -> Option<u32> {
        match e.kind {
            1 | 6 => Some(u32::from(self.data[e.at])),
            3 | 8 => Some(u32::from(self.u16(e.at))),
            4 | 9 => Some(self.u32(e.at)),
            _ => None,
        }
    }

    fn ratio(&self, e: &Entry) -> Option<(u32, u32)> {
        if e.kind != 5 && e.kind != 10 {
            return None;
        }
        let (num, den) = (self.u32(e.at), self.u32(e.at + 4));
        (den != 0).then_some((num, den))
    }

    fn float(&self, e: &Entry) -> Option<f32> {
        match self.ratio(e) {
            Some((num, den)) if e.kind == 10 => Some((f64::from(num as i32) / f64::from(den as i32)) as f32),
            Some((num, den)) => Some((f64::from(num) / f64::from(den)) as f32),
            None => self.uint(e).map(|v| v as f32),
        }
    }

    fn ascii(&self, e: &Entry) -> String {
        if e.kind != 2 {
            return String::new();
        }
        let mut out = String::new();
        for &c in &self.data[e.at..e.at + e.bytes.min(MAX_ASCII)] {
            if c == 0 {
                break;
            }
            if (0x20..=0x7e).contains(&c) {
                out.push(char::from(c));
            }
        }
        out.truncate(out.trim_end().len());
        out
    }

    /// The Exif SubIFD's offset, 0 when there is none.
    fn ifd(&self, at: usize, out: &mut Exif) -> usize {
        let count = usize::from(self.u16(at)).min(MAX_ENTRIES);
        let mut sub = 0;
        for i in 0..count {
            let Some(e) = self.entry(at + 2 + i * 12) else { continue };
            match e.tag {
                0x010F => out.make = self.ascii(&e),
                0x0110 => out.model = self.ascii(&e),
                0x0131 => out.software = self.ascii(&e),
                0xA434 => out.lens = self.ascii(&e),
                0x9003 => out.taken = self.ascii(&e),
                0x0132 if out.taken.is_empty() => out.taken = self.ascii(&e),
                0x0112 => {
                    if let Some(v @ 1..=8) = self.uint(&e) {
                        out.orientation = v as u8;
                    }
                }
                0x829A => out.shutter = self.ratio(&e),
                0x829D => out.aperture = self.float(&e),
                0x920A => out.focal_mm = self.float(&e),
                0x8827 | 0x8833 if out.iso.is_none() => out.iso = self.uint(&e),
                0xA405 => out.focal35_mm = self.uint(&e).map(|v| v as u16),
                0x9204 => out.exposure_bias = self.float(&e),
                0x9209 => out.flash = self.uint(&e).map(|v| v as u16),
                0x8769 => sub = self.uint(&e).unwrap_or(0) as usize,
                _ => {}
            }
        }
        sub
    }
}

fn parse_tiff(data: &[u8]) -> Option<Exif> {
    let big_endian = match data.get(..2)? {
        b"II" => false,
        b"MM" => true,
        _ => return None,
    };
    let tiff = Tiff { data, big_endian };
    if data.len() < 8 || tiff.u16(2) != 42 {
        return None;
    }
    let ifd0 = tiff.u32(4) as usize;
    if !tiff.has(ifd0, 2) {
        return None;
    }
    let mut out = Exif::default();
    let sub = tiff.ifd(ifd0, &mut out);
    if sub != 0 && sub != ifd0 && tiff.has(sub, 2) {
        tiff.ifd(sub, &mut out);
    }
    (!out.is_empty()).then_some(out)
}

fn jpeg_exif(data: &[u8]) -> Option<&[u8]> {
    let mut i = 2;
    while i + 4 <= data.len() {
        if data[i] != 0xFF {
            return None;
        }
        let marker = data[i + 1];
        if marker == 0xFF {
            i += 1;
            continue;
        }
        if marker == 0x01 || (0xD0..=0xD9).contains(&marker) {
            i += 2;
            continue;
        }
        if marker == 0xDA {
            return None;
        }
        let segment = usize::from(u16::from_be_bytes([data[i + 2], data[i + 3]]));
        if marker == 0xE1 && segment >= 8 && data.get(i + 4..i + 10) == Some(b"Exif\0\0") {
            let from = i + 10;
            return Some(&data[from..(from + segment - 8).min(data.len())]);
        }
        i += 2 + segment;
    }
    None
}

fn png_exif(data: &[u8]) -> Option<&[u8]> {
    let mut i = 8;
    while i + 8 <= data.len() {
        let length = u32::from_be_bytes([data[i], data[i + 1], data[i + 2], data[i + 3]]) as usize;
        match &data[i + 4..i + 8] {
            b"IDAT" => return None,
            b"eXIf" => {
                let from = i + 8;
                return Some(&data[from..from.saturating_add(length).min(data.len())]);
            }
            _ => {}
        }
        if length > data.len() {
            return None;
        }
        i += 12 + length;
    }
    None
}

fn le32(b: &[u8]) -> u32 {
    u32::from_le_bytes([b[0], b[1], b[2], b[3]])
}

fn is_webp(data: &[u8]) -> bool {
    data.len() >= 12 && &data[..4] == b"RIFF" && &data[8..12] == b"WEBP"
}

/// The spec has the chunk start at the TIFF header; some writers put the
/// "Exif\0\0" of a JPEG APP1 in front of it.
fn skip_exif_prefix(data: &[u8]) -> &[u8] {
    data.strip_prefix(b"Exif\0\0").unwrap_or(data)
}

/// The tags in `head`, the first bytes of a JPEG, PNG, TIFF or WebP file. A
/// head that stops inside the tags yields what it holds.
pub fn parse(head: &[u8]) -> Option<Exif> {
    if head.starts_with(&[0xFF, 0xD8]) {
        parse_tiff(jpeg_exif(head)?)
    } else if head.starts_with(b"\x89PNG\r\n\x1a\n") {
        parse_tiff(png_exif(head)?)
    } else if head.starts_with(b"II*\0") || head.starts_with(b"MM\0*") {
        parse_tiff(head)
    } else if is_webp(head) {
        let mut at = 12;
        while at + 8 <= head.len() {
            let length = le32(&head[at + 4..]) as usize;
            if &head[at..at + 4] == b"EXIF" {
                let from = at + 8;
                return parse_tiff(skip_exif_prefix(&head[from..from.saturating_add(length).min(head.len())]));
            }
            at = at.saturating_add(8 + length + (length & 1));
        }
        None
    } else {
        None
    }
}

/// A WebP's tags come after its pixels: whether `head` says there are some.
pub fn webp_has_exif(head: &[u8]) -> bool {
    is_webp(head) && head.len() >= 21 && &head[12..16] == b"VP8X" && head[20] & 0x08 != 0
}

/// A WebP's tags, from its chunk headers on: `read_at(offset, len)` reads the
/// file, fewer bytes at its end.
pub fn webp_exif(head: &[u8], mut read_at: impl FnMut(usize, usize) -> Option<Vec<u8>>) -> Option<Exif> {
    if !webp_has_exif(head) {
        return None;
    }
    let end = 8 + le32(&head[4..]) as usize;
    let mut at = 12;
    for _ in 0..MAX_WEBP_CHUNKS {
        if at + 8 > end {
            return None;
        }
        let header = read_at(at, 8)?;
        if header.len() < 8 {
            return None;
        }
        let length = le32(&header[4..]) as usize;
        if &header[..4] == b"EXIF" {
            let data = read_at(at + 8, length.min(MAX_WEBP_EXIF))?;
            return parse_tiff(skip_exif_prefix(&data));
        }
        at = at.checked_add(8 + length + (length & 1))?;
    }
    None
}
