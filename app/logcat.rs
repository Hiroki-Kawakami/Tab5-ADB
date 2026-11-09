/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::alloc::Global;
use alloc::collections::VecDeque;
use alloc::string::String;
use alloc::vec::Vec;
use core::alloc::Allocator;

pub const START_COMMAND: &str = "logcat -v threadtime -T 500";
const CHUNK_BYTES: usize = 256 * 1024;
const MAX_CHUNKS: usize = 8;
const MAX_LINES: usize = 16 * 1024;
const MAX_LINE: usize = 1024;
/// "MM-DD hh:mm:ss.mmm"
const TIME_LEN: usize = 18;

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub enum Level {
    Verbose,
    Debug,
    Info,
    Warning,
    Error,
    Fatal,
}

impl Level {
    /// The minimum levels to pick from; `Error` takes `Fatal` in.
    pub const PICKABLE: [Level; 5] = [Level::Verbose, Level::Debug, Level::Info, Level::Warning, Level::Error];

    fn from_byte(b: u8) -> Option<Self> {
        Some(match b {
            b'V' => Self::Verbose,
            b'D' => Self::Debug,
            b'I' => Self::Info,
            b'W' => Self::Warning,
            b'E' => Self::Error,
            b'F' | b'A' => Self::Fatal,
            _ => return None,
        })
    }

    pub fn letter(self) -> &'static str {
        match self {
            Self::Verbose => "V",
            Self::Debug => "D",
            Self::Info => "I",
            Self::Warning => "W",
            Self::Error => "E",
            Self::Fatal => "F",
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Self::Verbose => "Verbose",
            Self::Debug => "Debug",
            Self::Info => "Info",
            Self::Warning => "Warning",
            Self::Error => "Error",
            Self::Fatal => "Fatal",
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
struct Fields {
    pid: u32,
    tid: u32,
    tag: (u16, u16),
    message: u16,
}

#[derive(Clone, Copy)]
struct Entry {
    chunk: u32,
    off: u32,
    len: u16,
    level: Level,
    fields: Option<Fields>,
}

/// A stored line. One `logcat` did not format, a notice or a continuation,
/// has no header and is all message.
#[derive(Clone, Copy)]
pub struct Line<'a> {
    pub text: &'a str,
    pub level: Level,
    fields: Option<Fields>,
}

pub struct Header<'a> {
    pub time: &'a str,
    pub pid: u32,
    pub tid: u32,
    pub tag: &'a str,
}

impl<'a> Line<'a> {
    pub fn header(&self) -> Option<Header<'a>> {
        let f = self.fields?;
        Some(Header { time: &self.text[..TIME_LEN], pid: f.pid, tid: f.tid, tag: &self.text[usize::from(f.tag.0)..usize::from(f.tag.1)] })
    }

    pub fn message(&self) -> &'a str {
        self.fields.map_or(self.text, |f| &self.text[usize::from(f.message)..])
    }
}

/// "MM-DD hh:mm:ss.mmm  PID  TID L Tag: message", as `-v threadtime` prints.
fn parse(text: &str) -> Option<(Level, Fields)> {
    let b = text.as_bytes();
    if b.len() < TIME_LEN || !b[..TIME_LEN].is_ascii() || b[2] != b'-' || b[5] != b' ' || b[8] != b':' || b[11] != b':' || b[14] != b'.' {
        return None;
    }
    let mut at = TIME_LEN;
    let number = |at: &mut usize| -> Option<u32> {
        while b.get(*at) == Some(&b' ') {
            *at += 1;
        }
        let start = *at;
        while b.get(*at).is_some_and(u8::is_ascii_digit) {
            *at += 1;
        }
        text[start..*at].parse().ok()
    };
    let pid = number(&mut at)?;
    let tid = number(&mut at)?;
    if b.get(at) != Some(&b' ') {
        return None;
    }
    let level = Level::from_byte(*b.get(at + 1)?)?;
    if b.get(at + 2) != Some(&b' ') {
        return None;
    }
    let tag_start = at + 3;
    let (tag_end, message) = match text[tag_start..].find(": ") {
        Some(i) => (tag_start + i, tag_start + i + 2),
        None if text.ends_with(':') => (text.len() - 1, text.len()),
        None => return None,
    };
    let tag_end = tag_start + text[tag_start..tag_end].trim_end().len();
    Some((level, Fields { pid, tid, tag: (tag_start as u16, tag_end as u16), message: message as u16 }))
}

fn truncate(text: &str) -> &str {
    if text.len() <= MAX_LINE {
        return text;
    }
    let mut end = MAX_LINE;
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    &text[..end]
}

/// What the minimum level leaves through: `tag:X` the tag `X` in any case,
/// `pid:N` that process, anything else the lines holding it in any case.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Filter {
    All,
    Tag(String),
    Pid(u32),
    Text(String),
}

impl Filter {
    pub fn parse(input: &str) -> Self {
        let input = input.trim();
        if input.is_empty() {
            return Self::All;
        }
        let prefix = |p: &str| input.get(..p.len()).filter(|head| head.eq_ignore_ascii_case(p)).map(|_| input[p.len()..].trim());
        if let Some(tag) = prefix("tag:") {
            return Self::Tag(tag.into());
        }
        if let Some(pid) = prefix("pid:").and_then(|pid| pid.parse().ok()) {
            return Self::Pid(pid);
        }
        Self::Text(input.to_ascii_lowercase())
    }

    fn matches(&self, line: &Line) -> bool {
        match self {
            Self::All => true,
            Self::Tag(tag) => line.header().is_some_and(|h| h.tag.eq_ignore_ascii_case(tag)),
            Self::Pid(pid) => line.header().is_some_and(|h| h.pid == *pid),
            Self::Text(needle) => contains_ignore_case(line.text.as_bytes(), needle.as_bytes()),
        }
    }
}

/// `needle` is lowercase.
fn contains_ignore_case(hay: &[u8], needle: &[u8]) -> bool {
    hay.len() >= needle.len() && hay.windows(needle.len()).any(|w| w.iter().zip(needle).all(|(a, b)| a.to_ascii_lowercase() == *b))
}

/// The lines received, oldest first, and those the level and filter show.
/// Every line gets the next sequence number; the oldest go a chunk at a time
/// when the text or the line count is full.
pub struct Log<A: Allocator + Clone = Global> {
    alloc: A,
    chunks: VecDeque<Vec<u8, A>, A>,
    spare: Option<Vec<u8, A>>,
    first_chunk: u32,
    entries: VecDeque<Entry, A>,
    head_seq: u32,
    last_level: Level,
    last_time: Option<String>,
    resume_from: Option<String>,
    min_level: Level,
    filter: Filter,
    shown: VecDeque<u32, A>,
}

#[cfg(test)]
impl Log<Global> {
    pub fn new() -> Self {
        Self::new_in(Global)
    }
}

impl<A: Allocator + Clone> Log<A> {
    pub fn new_in(alloc: A) -> Self {
        Self {
            chunks: VecDeque::new_in(alloc.clone()),
            spare: None,
            first_chunk: 0,
            entries: VecDeque::new_in(alloc.clone()),
            head_seq: 0,
            last_level: Level::Info,
            last_time: None,
            resume_from: None,
            min_level: Level::Verbose,
            filter: Filter::All,
            shown: VecDeque::new_in(alloc.clone()),
            alloc,
        }
    }

    fn line(&self, entry: &Entry) -> Line<'_> {
        let chunk = &self.chunks[(entry.chunk - self.first_chunk) as usize];
        let bytes = &chunk[entry.off as usize..entry.off as usize + usize::from(entry.len)];
        Line { text: unsafe { core::str::from_utf8_unchecked(bytes) }, level: entry.level, fields: entry.fields }
    }

    pub fn get(&self, seq: u32) -> Option<Line<'_>> {
        let index = seq.checked_sub(self.head_seq)? as usize;
        self.entries.get(index).map(|e| self.line(e))
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn shown_len(&self) -> usize {
        self.shown.len()
    }

    /// The sequence number of the `index`th line shown.
    pub fn shown_seq(&self, index: usize) -> Option<u32> {
        self.shown.get(index).copied()
    }

    pub fn shown_index(&self, seq: u32) -> Option<usize> {
        self.shown.binary_search(&seq).ok()
    }

    pub fn min_level(&self) -> Level {
        self.min_level
    }

    /// The time of the last line received, which `clear` keeps.
    pub fn last_time(&self) -> Option<&str> {
        self.last_time.as_deref()
    }

    /// Until a line newer than `last_time` comes, what arrives is dropped
    /// when it is older or a line with the same time and text is kept: a
    /// stream restarted from that time sends those again.
    pub fn resume(&mut self) {
        self.resume_from = self.last_time.clone();
    }

    pub fn set_min_level(&mut self, level: Level) {
        self.min_level = level;
        self.refilter();
    }

    pub fn set_filter(&mut self, filter: Filter) {
        if filter != self.filter {
            self.filter = filter;
            self.refilter();
        }
    }

    fn passes(&self, line: &Line) -> bool {
        line.level >= self.min_level && self.filter.matches(line)
    }

    fn refilter(&mut self) {
        let mut shown = core::mem::replace(&mut self.shown, VecDeque::new_in(self.alloc.clone()));
        shown.clear();
        for (i, entry) in self.entries.iter().enumerate() {
            if self.passes(&self.line(entry)) {
                shown.push_back(self.head_seq + i as u32);
            }
        }
        self.shown = shown;
    }

    pub fn clear(&mut self) {
        self.head_seq += self.entries.len() as u32;
        self.entries.clear();
        self.shown.clear();
        self.first_chunk += self.chunks.len() as u32;
        if let Some(mut chunk) = self.chunks.pop_back() {
            chunk.clear();
            self.spare = Some(chunk);
        }
        self.chunks.clear();
    }

    /// Adds one line, without its end. Returns how many shown lines went to
    /// make room.
    pub fn push(&mut self, text: &str) -> usize {
        let text = truncate(text.trim_end_matches('\r'));
        let parsed = parse(text);
        if self.skip_resent(text, parsed.is_some()) {
            return 0;
        }
        let level = match parsed {
            Some((level, _)) => level,
            None => self.last_level,
        };
        self.store(text, level, parsed.map(|(_, f)| f))
    }

    /// Adds a line of the app's own, `[like this]`.
    pub fn push_notice(&mut self, text: &str) -> usize {
        self.store(truncate(text), Level::Info, None)
    }

    fn skip_resent(&mut self, text: &str, parsed: bool) -> bool {
        let Some(from) = self.resume_from.as_deref() else { return false };
        if !parsed {
            return true;
        }
        let time = &text[..TIME_LEN];
        if time < from {
            return true;
        }
        if time == from {
            let seen = self.entries.iter().rev().map(|e| self.line(e)).take_while(|l| l.header().is_none_or(|h| h.time == from)).any(|l| l.text == text);
            if seen {
                return true;
            }
        }
        self.resume_from = None;
        false
    }

    fn store(&mut self, text: &str, level: Level, fields: Option<Fields>) -> usize {
        let mut removed = 0;
        if self.entries.len() >= MAX_LINES {
            removed += self.evict();
        }
        let fits = self.chunks.back().is_some_and(|c| c.len() + text.len() <= CHUNK_BYTES);
        if !fits {
            if self.chunks.len() >= MAX_CHUNKS {
                removed += self.evict();
            }
            let chunk = match self.spare.take() {
                Some(chunk) => chunk,
                None => {
                    let mut chunk = Vec::new_in(self.alloc.clone());
                    if chunk.try_reserve_exact(CHUNK_BYTES).is_err() {
                        if self.chunks.len() < 2 {
                            return removed;
                        }
                        removed += self.evict();
                        chunk = self.spare.take().unwrap_or(chunk);
                    }
                    chunk
                }
            };
            self.chunks.push_back(chunk);
        }
        let chunk_no = self.first_chunk + self.chunks.len() as u32 - 1;
        let chunk = self.chunks.back_mut().unwrap();
        let entry = Entry { chunk: chunk_no, off: chunk.len() as u32, len: text.len() as u16, level, fields };
        chunk.extend_from_slice(text.as_bytes());
        self.entries.push_back(entry);
        if fields.is_some() {
            self.last_level = level;
            self.last_time = Some(text[..TIME_LEN].into());
        }
        let seq = self.head_seq + self.entries.len() as u32 - 1;
        if self.passes(&self.line(&entry)) {
            self.shown.push_back(seq);
        }
        removed
    }

    /// Drops the oldest chunk and its lines, or an eighth of the lines when
    /// they all share one. Returns how many shown lines went.
    fn evict(&mut self) -> usize {
        let count = if self.chunks.len() > 1 {
            let oldest = self.first_chunk;
            let count = self.entries.iter().take_while(|e| e.chunk == oldest).count();
            if let Some(mut chunk) = self.chunks.pop_front() {
                chunk.clear();
                self.spare = Some(chunk);
            }
            self.first_chunk += 1;
            count
        } else {
            MAX_LINES / 8
        };
        let count = count.min(self.entries.len());
        self.entries.drain(..count);
        self.head_seq += count as u32;
        let before = self.shown.len();
        while self.shown.front().is_some_and(|&seq| seq < self.head_seq) {
            self.shown.pop_front();
        }
        before - self.shown.len()
    }

    /// Every line, filter or not, each ending in a newline.
    pub fn write_to<B: Allocator>(&self, out: &mut Vec<u8, B>) {
        for entry in &self.entries {
            out.extend_from_slice(self.line(entry).text.as_bytes());
            out.push(b'\n');
        }
    }

    pub fn text_bytes(&self) -> usize {
        self.entries.iter().map(|e| usize::from(e.len) + 1).sum()
    }
}

/// Splits what a stream sends into lines, keeping a partial one for the next.
pub struct LineSplitter {
    carry: Vec<u8>,
}

impl LineSplitter {
    pub fn new() -> Self {
        Self { carry: Vec::new() }
    }

    pub fn feed(&mut self, data: &[u8], mut line: impl FnMut(&str)) {
        let mut rest = data;
        while let Some(end) = rest.iter().position(|&b| b == b'\n') {
            let piece = &rest[..end];
            if self.carry.is_empty() {
                line(&String::from_utf8_lossy(piece));
            } else {
                self.carry.extend_from_slice(piece);
                line(&String::from_utf8_lossy(&self.carry));
                self.carry.clear();
            }
            rest = &rest[end + 1..];
        }
        if self.carry.len() + rest.len() <= MAX_LINE * 4 {
            self.carry.extend_from_slice(rest);
        }
    }
}
