/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::collections::VecDeque;
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::ffi::CStr;
use core::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use core::time::Duration;

use agent_link::protocol::{CAP_AUDIO, ROTATION_RELEASE};
use agent_link::{AudioSource, Frame, FrameHead, Link, Touch, VideoStart};
use devkit_gfx::{Color, ImageFormat, Rect, Rotation, Target};
use devkit_ui::widgets::{divider, vertical_divider};
use devkit_ui::{Align, Bars, ColorRole, Component, Cx, Drag, Edge, Element, ExternalFrame, ExternalSurface, Handle, Justify, Policy, Presence, Prop, RenderMode, Screen, ScreenStyle, State, Strips, TouchPoint, Ui, id};
use idf::executor::JoinHandle;
use idf::cache::{self, Flags};
use idf::heap::Caps;
use idf::log::{error, info};
use idf::sync::{Mutex, Semaphore};
use idf::thread;
use idf::time::Instant;
use media_engine::{DecodeConfig, Decoder, Transform, Yuv, YuvRange, YuvStandard};

use crate::adb_link;
use crate::fonts::icon;
use crate::resources::{colors, icons};
use crate::phone_audio::PhoneAudio;
use crate::settings::{self, AudioPlayback, MirrorOrientation};
use crate::STRIP_LANES;
use crate::sram::Sram;

const TAG: &CStr = c"mirror";
const UI_STRIP_BYTES: usize = 128 * 1024;
const OPEN_TIMEOUT: Duration = Duration::from_secs(5);
const ROTATION_TIMEOUT: Duration = Duration::from_secs(2);
const ADAPT_TIMEOUT: Duration = Duration::from_secs(3);
/// How much of the box a fitted frame covers, in percent, for it to count as
/// filling it.
const FILLS_PERCENT: u32 = 98;
const IDLE: Duration = Duration::from_millis(20);
const STRIPS: u8 = 4;
const USB_MAX_FPS: u8 = 60;
const USB_MAX_RATE: u32 = 10_000_000;
const TCP_QUALITY: u8 = 70;
const TCP_MAX_FPS: u8 = 30;
/// How long a frame being drawn waits for its next strip.
const STRIP_TIMEOUT: Duration = Duration::from_millis(200);
const MAX_QUEUED_STRIPS: usize = 64;
const STATS_PERIOD: Duration = Duration::from_secs(1);
// On core 1 with the phone audio: above its decoder, which has a second of
// buffer, and below its player.
const PLAYER_PRIORITY: u32 = 13;
const PLAYER_CORE: u32 = 1;
const MAX_FRAME_BUFFERS: usize = 4;
const BLACK: Color = Color::rgb(0, 0, 0);
const BAR_THICKNESS: i16 = 72;
const BAR_MARGIN: i16 = 12;
const BAR_RADIUS: u8 = 20;
const BAR_SPACE: i16 = BAR_MARGIN + BAR_THICKNESS + BAR_MARGIN;
const KEY_SIZE: i16 = 64;
const REVEAL_CORNER: i16 = 96;
const REVEAL_DISTANCE: i32 = 48;

/// Android's `KeyEvent.KEYCODE_*`.
pub mod keycode {
    pub const HOME: u16 = 3;
    pub const BACK: u16 = 4;
    pub const VOLUME_UP: u16 = 24;
    pub const VOLUME_DOWN: u16 = 25;
    pub const POWER: u16 = 26;
    pub const APP_SWITCH: u16 = 187;
}


struct Shared {
    stop: AtomicBool,
    refresh: Mutex<Option<Rect>>,
    bar: AtomicBool,
    passthrough: AtomicBool,
    placed: Mutex<Option<Placed>>,
    display: AtomicU8,
    /// The box frames are sent for.
    box_size: Mutex<(u16, u16)>,
    wake: Semaphore,
    strips: Mutex<VecDeque<Frame>>,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum DisplayMode {
    Fit,
    Fill,
    Adapt,
}

impl DisplayMode {
    fn from_u8(v: u8) -> Self {
        match v {
            1 => Self::Fill,
            2 => Self::Adapt,
            _ => Self::Fit,
        }
    }
}

impl Shared {
    fn display(&self) -> DisplayMode {
        DisplayMode::from_u8(self.display.load(Ordering::Acquire))
    }

    fn redraw_all(&self) {
        *self.refresh.lock() = Some(Rect::new(0, 0, i16::MAX, i16::MAX));
        self.wake.give();
    }

    /// The first strip of the newest frame that can be drawn whole, dropping
    /// what comes before it.
    fn next_frame(&self) -> Option<Frame> {
        let mut strips = self.strips.lock();
        let mut dropped = Vec::new();
        loop {
            if strips.front().is_some_and(|s| s.head.y != 0) {
                dropped.extend(strips.pop_front());
            } else if strips.iter().filter(|s| s.head.last).count() >= 2 {
                while let Some(strip) = strips.pop_front() {
                    let last = strip.head.last;
                    dropped.push(strip);
                    if last {
                        break;
                    }
                }
            } else {
                break;
            }
        }
        let first = strips.pop_front();
        drop(strips);
        drop(dropped);
        first
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) struct Placed {
    pub(crate) rect: Rect,
    pub(crate) turn: Rotation,
    pub(crate) width: u16,
    pub(crate) height: u16,
    pub(crate) rotation: u8,
}

impl Placed {
    /// The frame's pixel under `(x, y)`, which may lie outside the frame.
    pub(crate) fn to_frame(&self, x: i16, y: i16) -> (i32, i32) {
        let (sx, sy) = (i32::from(x) - i32::from(self.rect.x), i32::from(y) - i32::from(self.rect.y));
        let (w, h) = (i32::from(self.width), i32::from(self.height));
        match self.turn {
            Rotation::R0 => (sx, sy),
            Rotation::R90 => (w - 1 - sy, sx),
            Rotation::R180 => (w - 1 - sx, h - 1 - sy),
            Rotation::R270 => (sy, h - 1 - sx),
        }
    }

    fn contains(&self, (fx, fy): (i32, i32)) -> bool {
        (0..i32::from(self.width)).contains(&fx) && (0..i32::from(self.height)).contains(&fy)
    }

    fn clamp(&self, (fx, fy): (i32, i32)) -> (u16, u16) {
        (fx.clamp(0, i32::from(self.width) - 1) as u16, fy.clamp(0, i32::from(self.height) - 1) as u16)
    }
}

struct Passthrough {
    link: Link,
    shared: Arc<Shared>,
    /// Fingers that reached the frame; the others wait until they do.
    entered: Vec<u8>,
    last: Option<Touch>,
    failing: bool,
}

impl Passthrough {
    fn touched(&mut self, points: &[TouchPoint], sampled_ms: u32) {
        let placed = *self.shared.placed.lock();
        let placed = placed.filter(|_| self.shared.passthrough.load(Ordering::Acquire));
        let mut touch = Touch { time_ms: sampled_ms, ..Touch::default() };
        let mut entered = Vec::new();
        if let Some(placed) = placed {
            (touch.width, touch.height, touch.rotation) = (placed.width, placed.height, placed.rotation);
            for p in points.iter().take(agent_link::protocol::MAX_TOUCHES) {
                let at = placed.to_frame(p.x, p.y);
                if self.entered.contains(&p.id) || placed.contains(at) {
                    let (x, y) = placed.clamp(at);
                    touch.points.push(agent_link::TouchPoint { id: p.id, x, y });
                    entered.push(p.id);
                }
            }
        }
        self.entered = entered;
        let same = match &self.last {
            Some(last) => (last.width, last.height, last.rotation, &last.points) == (touch.width, touch.height, touch.rotation, &touch.points),
            None => touch.points.is_empty(),
        };
        if same {
            return;
        }
        match self.link.touch(&touch) {
            Ok(()) => self.failing = false,
            Err(err) => {
                if !core::mem::replace(&mut self.failing, true) {
                    error!(TAG, "touch: {err}");
                }
            }
        }
        self.last = (!touch.points.is_empty()).then_some(touch);
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
struct Geometry {
    start: VideoStart,
    area: Rect,
    ui: Rotation,
}

fn quarters(rotation: Rotation) -> u8 {
    match rotation {
        Rotation::R0 => 0,
        Rotation::R90 => 1,
        Rotation::R180 => 2,
        Rotation::R270 => 3,
    }
}

fn from_quarters(q: u8) -> Rotation {
    match q & 3 {
        0 => Rotation::R0,
        1 => Rotation::R90,
        2 => Rotation::R180,
        _ => Rotation::R270,
    }
}

impl Geometry {
    fn of(frame: &ExternalFrame, mode: MirrorOrientation, fill: bool) -> Self {
        let area = frame.area;
        let (w, h) = (area.w as u16, area.h as u16);
        let tcp = matches!(adb_link::route(), Some(adb_link::Route::Tcp(_)));
        let (quality, max_fps, max_rate) = if tcp { (TCP_QUALITY, TCP_MAX_FPS, 0) } else { (0, USB_MAX_FPS, USB_MAX_RATE) };
        let start = VideoStart { box_width: w, box_height: h, swap: false, quality, max_fps, strips: STRIPS, fill, adapt: tcp, max_rate };
        let start = match mode {
            MirrorOrientation::Fit => start,
            MirrorOrientation::Device | MirrorOrientation::Rotate => {
                let (w, h) = if frame.rotation.swaps_axes() { (h, w) } else { (w, h) };
                VideoStart { box_width: w, box_height: h, swap: true, ..start }
            }
        };
        Self { start, area, ui: frame.rotation }
    }

    // Surface.ROTATION_* turns the picture clockwise and Rotation counter-clockwise.
    fn device_rotation(&self, mode: MirrorOrientation) -> Option<u8> {
        (mode == MirrorOrientation::Rotate).then(|| (4 - quarters(self.ui)) & 3)
    }

    fn turn(&self, mode: MirrorOrientation, device: u8) -> Rotation {
        match mode {
            MirrorOrientation::Fit => Rotation::R0,
            MirrorOrientation::Device | MirrorOrientation::Rotate => from_quarters(8 - device - quarters(self.ui)),
        }
    }
}

async fn receive(link: Link, shared: Arc<Shared>, start: VideoStart, rotation: Option<u8>) {
    if let Some(rotation) = rotation
        && let Err(err) = link.set_rotation(rotation, ROTATION_TIMEOUT).await
    {
        error!(TAG, "set rotation: {err}");
    }
    let mut video = match link.open_video(&start, OPEN_TIMEOUT).await {
        Ok(video) => video,
        Err(err) => {
            error!(TAG, "open video: {err}");
            return;
        }
    };
    info!(TAG, "box {}x{}{}", start.box_width, start.box_height, if start.swap { " swap" } else { "" });
    loop {
        let strip = match video.next_frame().await {
            Ok(strip) => strip,
            Err(err) => {
                error!(TAG, "video: {err}");
                return;
            }
        };
        let old = {
            let mut strips = shared.strips.lock();
            strips.push_back(strip);
            (strips.len() > MAX_QUEUED_STRIPS).then(|| strips.pop_front()).flatten()
        };
        drop(old);
        shared.wake.give();
    }
}

struct Player {
    surface: ExternalSurface,
    shared: Arc<Shared>,
    link: Link,
    mode: MirrorOrientation,
    decoder: Option<(ImageFormat, Decoder)>,
    shown: Vec<Frame>,
    geometry: Option<Geometry>,
    session: Option<JoinHandle<()>>,
    blacked: [Option<Rect>; MAX_FRAME_BUFFERS],
    /// Per framebuffer, the `seq` of the frame it holds whole.
    held: [u32; MAX_FRAME_BUFFERS],
    /// Per framebuffer, what to write again.
    stale: [Rect; MAX_FRAME_BUFFERS],
    seq: u32,
    stats: (Instant, u32, usize),
    handle: Handle<MirrorMsg>,
    /// Whether fitted frames fill the box, as last told to the screen.
    fills: Option<bool>,
}

impl Player {
    fn run(&mut self, sram: &mut Sram) {
        while !self.shared.stop.load(Ordering::Acquire) {
            self.shared.wake.take_timeout(IDLE);
            let refresh = self.shared.refresh.lock().take();
            if let Some(area) = refresh {
                self.stale.iter_mut().for_each(|s| *s = s.union(&area));
            }
            let first = self.shared.next_frame();
            if first.is_none() && refresh.is_none() && self.geometry.is_some() {
                continue;
            }
            let Some(frame) = self.surface.begin() else {
                if let Some(first) = first {
                    self.shared.strips.lock().push_front(first);
                }
                continue;
            };
            let geometry = Geometry::of(&frame, self.mode, self.shared.display() == DisplayMode::Fill);
            let reopen = self.geometry.is_none_or(|g| g.start != geometry.start || g.device_rotation(self.mode) != geometry.device_rotation(self.mode));
            if self.geometry != Some(geometry) {
                self.geometry = Some(geometry);
                self.blacked = [None; MAX_FRAME_BUFFERS];
                *self.shared.box_size.lock() = (geometry.start.box_width, geometry.start.box_height);
            }
            if reopen {
                self.restart(geometry);
            }
            let fresh = first.is_some();
            let index = buffer(&frame);
            let drawn = match first {
                Some(first) => self.draw_new(&frame, first, sram),
                None => self.redraw(&frame, sram),
            };
            match drawn {
                Ok(true) => {
                    self.stale[index] = Rect::default();
                    frame.end();
                    if fresh {
                        self.count();
                    }
                }
                Ok(false) => {}
                Err(err) => error!(TAG, "draw: {err}"),
            }
        }
        if let Some(session) = self.session.take() {
            session.abort();
        }
    }

    fn restart(&mut self, geometry: Geometry) {
        if let Some(session) = self.session.take() {
            session.abort();
        }
        let Some(spawner) = adb_link::spawner() else {
            error!(TAG, "adb not started");
            return;
        };
        let rotation = geometry.device_rotation(self.mode);
        self.session = Some(spawner.spawn(receive(self.link.clone(), self.shared.clone(), geometry.start, rotation)));
    }

    /// Where the whole frame lands; `None` when there is nothing to show in
    /// this geometry: a frame sent for the box before the screen turned may
    /// not fit.
    fn place(&mut self, frame: &ExternalFrame, head: &FrameHead) -> idf::Result<Option<(Rect, Rotation, bool)>> {
        let Some(geometry) = self.geometry else {
            *self.shared.placed.lock() = None;
            return Ok(None);
        };
        self.check_fills(&geometry.start, head);
        let area = frame.area;
        let turn = geometry.turn(self.mode, head.rotation);
        let (w, h) = (head.width as i16, head.height as i16);
        let (w, h) = if turn.swaps_axes() { (h, w) } else { (w, h) };
        let (mut x, mut y) = (area.x + (area.w - w) / 2, area.y + (area.h - h) / 2);
        if self.shared.bar.load(Ordering::Acquire) {
            if area.w < area.h {
                x = x.max(area.x + BAR_SPACE).min(area.right() - w);
            } else {
                y = y.min(area.bottom() - BAR_SPACE - h).max(area.y);
            }
        }
        let rect = Rect::new(x, y, w, h);
        if !area.contains(&rect) {
            *self.shared.placed.lock() = None;
            return Ok(None);
        }
        *self.shared.placed.lock() = Some(Placed { rect, turn, width: head.width, height: head.height, rotation: head.rotation });
        let index = buffer(frame);
        let moved = self.blacked[index] != Some(rect);
        let stale = if moved { area } else { self.stale[index] };
        let mut target = frame.target().ok_or(idf::Error::INVALID_ARG)?;
        for margin in margins(area, rect).into_iter().map(|m| m.intersect(&stale)).filter(|r| !r.is_empty()) {
            clear(&mut target, margin)?;
            frame.wrote(margin);
        }
        self.blacked[index] = Some(rect);
        Ok(Some((rect, turn, moved)))
    }

    fn check_fills(&mut self, start: &VideoStart, head: &FrameHead) {
        if self.shared.display() != DisplayMode::Fit {
            return;
        }
        let (bw, bh) = (u32::from(start.box_width), u32::from(start.box_height));
        let (bw, bh) = if start.swap && head.rotation & 1 != 0 { (bh, bw) } else { (bw, bh) };
        let fills = u32::from(head.width) * 100 >= bw * FILLS_PERCENT && u32::from(head.height) * 100 >= bh * FILLS_PERCENT;
        if self.fills != Some(fills) && self.handle.post(MirrorMsg::Fills(fills)) {
            self.fills = Some(fills);
        }
    }

    fn render(&mut self, frame: &ExternalFrame, strip: &Frame, (rect, turn): (Rect, Rotation), sram: &mut Sram) -> idf::Result<()> {
        let mut transform = Transform::new();
        transform.rotation = turn;
        let at = strip_rect(&strip.head, rect, turn);
        transform.at = (at.x, at.y);
        frame.wrote(at);
        let mut target = frame.target().ok_or(idf::Error::INVALID_ARG)?;
        let decoder = decoder_for(&mut self.decoder, frame.fb.format())?;
        let bytes = sram.as_mut();
        // The decoder refuses strip buffers off a 64-byte boundary.
        let (a, b) = bytes.split_at_mut(bytes.len() / 2 / 64 * 64);
        decoder.render(&strip.jpeg.parts(), &mut [a, b], &transform, &mut target)?.wait()?;
        self.stats.2 += strip.jpeg.len();
        Ok(())
    }

    /// Draws strips as they arrive; `false` when the frame did not come
    /// whole.
    fn draw_new(&mut self, frame: &ExternalFrame, first: Frame, sram: &mut Sram) -> idf::Result<bool> {
        let mut drawn = Vec::new();
        let complete = self.draw_strips(frame, first, &mut drawn, sram);
        if matches!(complete, Ok(true)) {
            self.shown = drawn;
            self.seq = self.seq.wrapping_add(1);
            self.held[buffer(frame)] = self.seq;
        }
        complete
    }

    fn draw_strips(&mut self, frame: &ExternalFrame, first: Frame, drawn: &mut Vec<Frame>, sram: &mut Sram) -> idf::Result<bool> {
        let Some((rect, turn, _)) = self.place(frame, &first.head)? else {
            drawn.push(first);
            return Ok(false);
        };
        let placed = (rect, turn);
        let mut strip = first;
        loop {
            let rendered = self.render(frame, &strip, placed, sram);
            let last = strip.head.last;
            drawn.push(strip);
            rendered?;
            if last {
                return Ok(true);
            }
            strip = match self.next_strip() {
                Some(next) if next.head.y != 0 => next,
                Some(next) => {
                    self.shared.strips.lock().push_front(next);
                    return Ok(false);
                }
                None => return Ok(false),
            };
        }
    }

    fn next_strip(&self) -> Option<Frame> {
        let start = Instant::now();
        loop {
            if let Some(strip) = self.shared.strips.lock().pop_front() {
                return Some(strip);
            }
            let left = STRIP_TIMEOUT.saturating_sub(start.elapsed());
            if left.is_zero() || self.shared.stop.load(Ordering::Acquire) {
                return None;
            }
            self.shared.wake.take_timeout(left);
        }
    }

    fn redraw(&mut self, frame: &ExternalFrame, sram: &mut Sram) -> idf::Result<bool> {
        let shown = core::mem::take(&mut self.shown);
        let result = (|| {
            let index = buffer(frame);
            let whole = self.held[index] != self.seq;
            let stale = self.stale[index];
            let placed = match shown.first() {
                Some(first) => self.place(frame, &first.head)?,
                None => {
                    *self.shared.placed.lock() = None;
                    None
                }
            };
            let Some((rect, turn, moved)) = placed else {
                self.blank(frame)?;
                return Ok(true);
            };
            for strip in &shown {
                if whole || moved || strip_rect(&strip.head, rect, turn).intersects(&stale) {
                    self.render(frame, strip, (rect, turn), sram)?;
                }
            }
            self.held[index] = self.seq;
            Ok(true)
        })();
        self.shown = shown;
        result
    }

    fn blank(&mut self, frame: &ExternalFrame) -> idf::Result<()> {
        let index = buffer(frame);
        let area = frame.area;
        let dirty = if self.blacked[index] == Some(Rect::default()) { self.stale[index].intersect(&area) } else { area };
        if !dirty.is_empty() {
            let mut target = frame.target().ok_or(idf::Error::INVALID_ARG)?;
            clear(&mut target, dirty)?;
            frame.wrote(dirty);
        }
        self.blacked[index] = Some(Rect::default());
        Ok(())
    }

    fn count(&mut self) {
        self.stats.1 += 1;
        let elapsed = self.stats.0.elapsed();
        if elapsed >= STATS_PERIOD {
            let ms = elapsed.as_millis() as u32;
            info!(TAG, "{} fps, {} KB/s", self.stats.1 * 1000 / ms, self.stats.2 as u32 / ms);
            self.stats = (Instant::now(), 0, 0);
        }
    }
}

fn buffer(frame: &ExternalFrame) -> usize {
    frame.index.min(MAX_FRAME_BUFFERS - 1)
}

/// Where `head`'s rows land in a frame placed at `rect`, turned by `turn`.
pub(crate) fn strip_rect(head: &FrameHead, rect: Rect, turn: Rotation) -> Rect {
    let trims = [0, head.y as i16, 0, head.height as i16 - head.y as i16 - head.rows as i16];
    let k = usize::from(quarters(turn));
    let (w, h) = (head.width as i16, head.rows as i16);
    let (w, h) = if turn.swaps_axes() { (h, w) } else { (w, h) };
    Rect::new(rect.x + trims[k % 4], rect.y + trims[(1 + k) % 4], w, h)
}

fn decoder_for(decoder: &mut Option<(ImageFormat, Decoder)>, format: ImageFormat) -> idf::Result<&mut Decoder> {
    if decoder.as_ref().is_none_or(|(f, _)| *f != format) {
        let yuv = Yuv { range: YuvRange::Full, standard: YuvStandard::Bt601 };
        *decoder = Some((format, Decoder::new(&DecodeConfig { format, yuv })?));
    }
    decoder.as_mut().map(|(_, d)| d).ok_or(idf::Error::INVALID_STATE)
}

// PPA fills of large areas starve the DSI of PSRAM bandwidth and underrun it.
fn clear(target: &mut Target<'_>, rect: Rect) -> idf::Result<()> {
    let stored = target.stored_rect(rect).ok_or(idf::Error::INVALID_ARG)?;
    let (stride, format) = (target.stride_bytes(), target.format());
    let first = stored.y as usize * stride + format.row_bytes(stored.x as usize);
    let row = format.row_bytes(stored.w as usize);
    let base = target.as_ptr();
    for y in 0..stored.h as usize {
        unsafe { core::ptr::write_bytes(base.add(first + y * stride), 0, row) };
    }
    unsafe { cache::msync(base.add(first), (stored.h as usize - 1) * stride + row, Flags::C2M | Flags::UNALIGNED) }
}

fn margins(area: Rect, inner: Rect) -> [Rect; 4] {
    [
        Rect::new(area.x, area.y, area.w, inner.y - area.y),
        Rect::new(area.x, inner.bottom(), area.w, area.bottom() - inner.bottom()),
        Rect::new(area.x, inner.y, inner.x - area.x, inner.h),
        Rect::new(inner.right(), inner.y, area.right() - inner.right(), inner.h),
    ]
}

#[derive(Clone, Copy)]
pub enum MirrorMsg {
    Back,
    Started,
    Restored,
    Hide,
    Reveal(Drag),
    Passthrough,
    Key(u16, bool),
    DisplayMode,
    Fills(bool),
}

fn key<const CODE: u16>(drag: Drag) -> MirrorMsg {
    MirrorMsg::Key(CODE, !drag.ended)
}

#[derive(Clone, Copy)]
enum Action {
    Tap(MirrorMsg),
    Key(fn(Drag) -> MirrorMsg),
    Disabled,
}

pub struct MirrorScreen {
    link: Link,
    mode: MirrorOrientation,
    surface: ExternalSurface,
    shared: Arc<Shared>,
    player: Option<thread::JoinHandle<Sram>>,
    switching: bool,
    leaving: bool,
    bar: bool,
    passthrough: bool,
    /// Keys pressed, so that a held finger sends one press.
    keys: Vec<u16>,
    display: DisplayMode,
    /// An `adapt_display` is on its way.
    adapting: bool,
    /// The device keeps a resolution the user set, so Adapt is skipped.
    adapt_refused: bool,
    /// Fitted frames already fill the box, so the modes look the same.
    fills: bool,
    swipe: Option<(i16, i16)>,
    /// The surface is in the view; screen moves cannot draw over it.
    live: bool,
    audio: Option<PhoneAudio>,
}

impl MirrorScreen {
    pub fn new(link: Link) -> Option<Self> {
        let surface = match ExternalSurface::new() {
            Ok(surface) => surface,
            Err(err) => {
                error!(TAG, "surface: {err}");
                return None;
            }
        };
        surface.set_policy(Policy::Stamp);
        let shared = Arc::new(Shared {
            stop: AtomicBool::new(false),
            refresh: Mutex::new(None),
            bar: AtomicBool::new(true),
            passthrough: AtomicBool::new(true),
            placed: Mutex::new(None),
            display: AtomicU8::new(DisplayMode::Fit as u8),
            box_size: Mutex::new((0, 0)),
            wake: Semaphore::new(0, 1),
            strips: Mutex::new(VecDeque::new()),
        });
        let refresh = shared.clone();
        surface.set_on_refresh(move |area| {
            let mut pending = refresh.refresh.lock();
            *pending = Some(pending.map_or(area, |p| p.union(&area)));
            drop(pending);
            refresh.wake.give();
        });
        let mut passthrough = Passthrough { link: link.clone(), shared: shared.clone(), entered: Vec::new(), last: None, failing: false };
        surface.set_on_touch(move |points, sampled_ms| passthrough.touched(points, sampled_ms));
        Some(Self { link, mode: settings::mirror_orientation(), surface, shared, player: None, switching: false, leaving: false, bar: true, passthrough: true, keys: Vec::new(), display: DisplayMode::Fit, adapting: false, adapt_refused: false, fills: false, swipe: None, live: false, audio: None })
    }

    fn start(&mut self, cx: &mut Cx<Self>) {
        let strips = match Strips::allocate(UI_STRIP_BYTES, Caps::SPIRAM) {
            Ok(strips) => strips,
            Err(err) => {
                error!(TAG, "strips: {err}");
                return;
            }
        };
        cx.set_render_mode(RenderMode { strips, lanes: 1, display: Default::default() }, MirrorMsg::Started);
        self.switching = true;
    }

    fn play(&mut self, ui: Sram, handle: Handle<MirrorMsg>) {
        drop(ui);
        let Some(mut sram) = crate::sram::mirror() else {
            error!(TAG, "strips: taken");
            return;
        };
        self.shared.stop.store(false, Ordering::Release);
        let mut player = Player {
            surface: self.surface.clone(),
            shared: self.shared.clone(),
            link: self.link.clone(),
            mode: self.mode,
            decoder: None,
            shown: Vec::new(),
            geometry: None,
            session: None,
            blacked: [None; MAX_FRAME_BUFFERS],
            held: [0; MAX_FRAME_BUFFERS],
            stale: [Rect::default(); MAX_FRAME_BUFFERS],
            seq: 0,
            stats: (Instant::now(), 0, 0),
            handle,
            fills: None,
        };
        let spawned = thread::Builder::new().name("mirror").stack_size(8192).priority(PLAYER_PRIORITY).core(Some(PLAYER_CORE)).spawn(move || {
            player.run(&mut sram);
            sram
        });
        match spawned {
            Ok(player) => self.player = Some(player),
            Err(err) => error!(TAG, "player: {err}"),
        }
    }

    fn stop(&mut self) -> Option<Sram> {
        let player = self.player.take()?;
        self.shared.stop.store(true, Ordering::Release);
        self.shared.wake.give();
        let sram = player.join();
        *self.shared.placed.lock() = None;
        Some(sram)
    }

    fn restore(&mut self, mirror: Sram, cx: &mut Cx<Self>) {
        drop(mirror);
        let Some(sram) = crate::sram::ui() else {
            error!(TAG, "strips: taken");
            return;
        };
        cx.set_render_mode(RenderMode { strips: Strips::new(sram), lanes: STRIP_LANES, display: Default::default() }, MirrorMsg::Restored);
    }

    fn show_bar(&mut self, shown: bool) {
        self.bar = shown;
        self.shared.bar.store(shown, Ordering::Release);
        self.shared.redraw_all();
    }

    fn set_display(&mut self, display: DisplayMode) {
        if self.display == DisplayMode::Adapt && display != DisplayMode::Adapt {
            self.release_display();
        }
        self.display = display;
        self.shared.display.store(display as u8, Ordering::Release);
        self.shared.redraw_all();
    }

    fn next_display(&mut self, cx: &mut Cx<Self>) {
        if self.adapting {
            return;
        }
        match self.display {
            DisplayMode::Fit => self.set_display(DisplayMode::Fill),
            DisplayMode::Fill if self.adapt_refused => self.set_display(DisplayMode::Fit),
            DisplayMode::Fill => {
                let (w, h) = *self.shared.box_size.lock();
                let link = self.link.clone();
                self.adapting = true;
                cx.spawn(move |this| async move {
                    let result = link.adapt_display(w, h, ADAPT_TIMEOUT).await;
                    this.with(|s, cx| s.adapted(result, cx));
                });
            }
            DisplayMode::Adapt => self.set_display(DisplayMode::Fit),
        }
    }

    fn adapted(&mut self, result: agent_link::Result<()>, cx: &mut Cx<Self>) {
        self.adapting = false;
        if !self.live {
            return;
        }
        match result {
            Ok(()) => self.set_display(DisplayMode::Adapt),
            Err(agent_link::Error::Status(agent_link::protocol::ESTATE)) => {
                info!(TAG, "the device keeps a resolution the user set; no Adapt");
                self.adapt_refused = true;
                self.set_display(DisplayMode::Fit);
            }
            Err(err) => {
                error!(TAG, "adapt display: {err}");
                self.set_display(DisplayMode::Fit);
            }
        }
        cx.rebuild();
    }

    fn release_display(&self) {
        let Some(spawner) = adb_link::spawner() else { return };
        let link = self.link.clone();
        drop(spawner.spawn(async move {
            if let Err(err) = link.adapt_display(0, 0, ADAPT_TIMEOUT).await {
                error!(TAG, "release display: {err}");
            }
        }));
    }

    /// `true` when the view changed.
    fn reveal(&mut self, drag: Drag) -> bool {
        if drag.ended {
            return self.swipe.take().is_some();
        }
        let (x, y) = *self.swipe.get_or_insert((drag.x, drag.y));
        let (dx, dy) = (i32::from(drag.x - x), i32::from(drag.y - y));
        if !self.bar && dx * dx + dy * dy >= REVEAL_DISTANCE * REVEAL_DISTANCE {
            self.show_bar(true);
            return true;
        }
        false
    }

    fn release_rotation(&self) {
        if self.mode != MirrorOrientation::Rotate {
            return;
        }
        let Some(spawner) = adb_link::spawner() else { return };
        let link = self.link.clone();
        drop(spawner.spawn(async move {
            if let Err(err) = link.set_rotation(ROTATION_RELEASE, ROTATION_TIMEOUT).await {
                error!(TAG, "release rotation: {err}");
            }
        }));
    }
}

impl Drop for MirrorScreen {
    fn drop(&mut self) {
        if self.stop().is_some() {
            self.release_rotation();
        }
        if self.display == DisplayMode::Adapt || self.adapting {
            self.release_display();
        }
    }
}

impl Component for MirrorScreen {
    type Msg = MirrorMsg;

    fn update(&mut self, msg: MirrorMsg, cx: &mut Cx<Self>) {
        match msg {
            MirrorMsg::Back => {
                self.live = false;
                cx.rebuild();
                cx.nav().pop();
            }
            MirrorMsg::Started => {
                self.switching = false;
                let Some(sram) = cx.take_strips().and_then(Strips::into_inner::<Sram>) else {
                    error!(TAG, "display refused the mirror mode");
                    return;
                };
                if self.leaving {
                    self.restore(sram, cx);
                } else {
                    self.play(sram, cx.handle());
                }
            }
            MirrorMsg::Restored => drop(cx.take_strips()),
            MirrorMsg::Hide => {
                self.show_bar(false);
                cx.rebuild();
            }
            MirrorMsg::Reveal(drag) => {
                if self.reveal(drag) {
                    cx.rebuild();
                }
            }
            MirrorMsg::Key(code, down) => {
                let held = self.keys.contains(&code);
                if down == held {
                    return;
                }
                if down {
                    self.keys.push(code);
                } else {
                    self.keys.retain(|&k| k != code);
                }
                if let Err(err) = self.link.key(code, down) {
                    error!(TAG, "key {code}: {err}");
                }
            }
            MirrorMsg::DisplayMode => self.next_display(cx),
            MirrorMsg::Fills(fills) => {
                if self.fills != fills {
                    self.fills = fills;
                    cx.rebuild();
                }
            }
            MirrorMsg::Passthrough => {
                self.passthrough = !self.passthrough;
                self.shared.passthrough.store(self.passthrough, Ordering::Release);
                cx.rebuild();
            }
        }
    }

    fn view<'a>(&'a self, ui: &'a Ui<MirrorMsg>) -> Element<'a, MirrorMsg> {
        let mut children = Vec::new();
        children.push(if self.live { ui.external(&self.surface).grow(1).id(id!("mirror")) } else { ui.column([]).grow(1) });
        // The corner stays while its swipe lasts, so the press that shows the bar is not a tap on it.
        if !self.bar || self.swipe.is_some() {
            children.push(ui.column([]).size(REVEAL_CORNER, REVEAL_CORNER).absolute(Some(0), None, None, Some(0)).on_drag(MirrorMsg::Reveal).id(id!("reveal")));
        }
        if self.bar {
            let (w, h) = ui.screen_size();
            let reach = BAR_MARGIN + BAR_THICKNESS;
            let presence = if w < h { Presence::from_edge(Edge::Left).offset(0, reach) } else { Presence::from_edge(Edge::Bottom).offset(-reach, 0) };
            let display = !(self.fills && self.display == DisplayMode::Fit);
            let bar = control_bar(ui, w < h, self.passthrough, display).absolute(Some(BAR_MARGIN), None, None, Some(BAR_MARGIN));
            children.push(if self.live { bar.appear(presence) } else { bar }.id(id!("controls")));
        }
        ui.column(children).background(BLACK)
    }
}

fn control_bar<'a>(ui: &'a Ui<MirrorMsg>, vertical: bool, passthrough: bool, display: bool) -> Element<'a, MirrorMsg> {
    let display = if display { Action::Tap(MirrorMsg::DisplayMode) } else { Action::Disabled };
    let last = [(icons::TOUCH, Action::Tap(MirrorMsg::Passthrough)), (icons::DISPLAY_MODE, display), (icons::CLOSE, Action::Tap(MirrorMsg::Back))];
    let mut groups: [&[(char, Action)]; 4] = [
        &[(icons::HIDE, Action::Tap(MirrorMsg::Hide))],
        &[(icons::NAV_BACK, Action::Key(key::<{ keycode::BACK }>)), (icons::NAV_HOME, Action::Key(key::<{ keycode::HOME }>)), (icons::NAV_RECENTS, Action::Key(key::<{ keycode::APP_SWITCH }>))],
        &[(icons::VOLUME_DOWN, Action::Key(key::<{ keycode::VOLUME_DOWN }>)), (icons::VOLUME_UP, Action::Key(key::<{ keycode::VOLUME_UP }>)), (icons::POWER, Action::Key(key::<{ keycode::POWER }>))],
        &last,
    ];
    if vertical {
        groups.reverse();
    }
    let mut items = Vec::new();
    for (i, group) in groups.iter().enumerate() {
        if i > 0 {
            items.push(if vertical { divider(ui).padding_x(ui.theme().widgets.unit.gap) } else { vertical_divider(ui) });
        }
        let mut keys: Vec<_> = group.iter().map(|&(glyph, action)| control_key(ui, glyph, action, passthrough)).collect();
        if vertical {
            keys.reverse();
        }
        items.extend(keys);
    }
    let pad = (BAR_THICKNESS - KEY_SIZE) / 2;
    let bar = if vertical { ui.column(items).width(BAR_THICKNESS).padding_y(pad) } else { ui.row(items).height(BAR_THICKNESS).padding_x(pad) };
    bar.align(Align::Center).radius(BAR_RADIUS).background(ui.colors().surface)
}

fn control_key<'a>(ui: &'a Ui<MirrorMsg>, glyph: char, action: Action, passthrough: bool) -> Element<'a, MirrorMsg> {
    let color = match action {
        Action::Tap(MirrorMsg::Back) => colors::DESTRUCTIVE,
        Action::Tap(MirrorMsg::Passthrough) if passthrough => ColorRole::Primary,
        Action::Disabled => colors::MUTED,
        _ => ColorRole::Text,
    };
    let key = ui
        .row([ui.icon(icon(glyph)).foreground(color)])
        .size(KEY_SIZE, KEY_SIZE)
        .radius(14)
        .align(Align::Center)
        .justify(Justify::Center)
        .when(State::PRESSED, &[Prop::Background(colors::PRESSED)]);
    match action {
        Action::Tap(msg) => key.on_tap(msg),
        Action::Key(on_drag) => key.on_drag(on_drag),
        Action::Disabled => key,
    }
}

impl Screen for MirrorScreen {
    fn style(&self) -> ScreenStyle {
        ScreenStyle { bars: Bars::Hidden, ..ScreenStyle::default() }
    }

    fn did_appear(&mut self, cx: &mut Cx<Self>) {
        self.leaving = false;
        self.live = true;
        if self.audio.is_none() && settings::mirror_playback() == AudioPlayback::Tab5 && self.link.supports(CAP_AUDIO) {
            self.audio = PhoneAudio::start(self.link.clone(), AudioSource::All, || ());
        }
        cx.rebuild();
        if !self.switching && self.player.is_none() {
            self.start(cx);
        }
    }

    fn will_disappear(&mut self, cx: &mut Cx<Self>) {
        self.live = false;
        self.audio = None;
        cx.rebuild();
        self.keys.clear();
        if let Err(err) = self.link.input_clear() {
            error!(TAG, "input clear: {err}");
        }
        if self.switching {
            self.leaving = true;
            return;
        }
        if self.adapting {
            self.release_display();
        }
        self.set_display(DisplayMode::Fit);
        if let Some(sram) = self.stop() {
            self.release_rotation();
            self.restore(sram, cx);
        }
    }
}
