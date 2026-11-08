/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::collections::VecDeque;
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::ffi::CStr;
use core::sync::atomic::{AtomicBool, Ordering};
use core::time::Duration;

use agent_link::protocol::ROTATION_RELEASE;
use agent_link::{Frame, FrameHead, Jpeg, Link, Touch, VideoStart};
use devkit::Lease;
use devkit_gfx::{Color, ImageFormat, Rect, Rotation, Target};
use devkit_ui::widgets::{divider, vertical_divider};
use devkit_ui::{Align, Bars, ColorRole, Component, Cx, Drag, Edge, Element, ExternalFrame, ExternalSurface, Justify, Policy, Presence, Prop, RenderMode, Screen, ScreenStyle, State, Strips, TouchPoint, Ui, id};
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
use crate::settings::{self, MirrorOrientation};
use crate::{STRIP_LANES, SharedSram};

const TAG: &CStr = c"mirror";
const UI_STRIP_BYTES: usize = 128 * 1024;
const OPEN_TIMEOUT: Duration = Duration::from_secs(5);
const ROTATION_TIMEOUT: Duration = Duration::from_secs(2);
const IDLE: Duration = Duration::from_millis(20);
const STRIPS: u8 = 4;
/// How long a frame being drawn waits for its next strip.
const STRIP_TIMEOUT: Duration = Duration::from_millis(200);
const MAX_QUEUED_STRIPS: usize = 64;
const STATS_PERIOD: Duration = Duration::from_secs(1);
const MAX_FRAME_BUFFERS: usize = 4;
const BLACK: Color = Color::rgb(0, 0, 0);
const BAR_THICKNESS: i16 = 72;
const BAR_MARGIN: i16 = 12;
const BAR_RADIUS: u8 = 20;
const BAR_SPACE: i16 = BAR_MARGIN + BAR_THICKNESS + BAR_MARGIN;
const KEY_SIZE: i16 = 64;
const REVEAL_CORNER: i16 = 96;
const REVEAL_DISTANCE: i32 = 48;

type Spare = Vec<u8, Caps>;
type Sram = Lease<'static, SharedSram>;

struct Shared {
    stop: AtomicBool,
    refresh: Mutex<Option<Rect>>,
    bar: AtomicBool,
    passthrough: AtomicBool,
    placed: Mutex<Option<Placed>>,
    wake: Semaphore,
    strips: Mutex<VecDeque<Frame<Caps>>>,
    spare: Mutex<Vec<Spare>>,
}

impl Shared {
    fn recycle(&self, strip: Frame<Caps>) {
        if let Jpeg::Copied(vec) = strip.jpeg {
            self.spare.lock().push(vec);
        }
    }

    /// The first strip of the newest frame that can be drawn whole, dropping
    /// what comes before it.
    fn next_frame(&self) -> Option<Frame<Caps>> {
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
        dropped.into_iter().for_each(|s| self.recycle(s));
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
    fn of(frame: &ExternalFrame, mode: MirrorOrientation) -> Self {
        let area = frame.area;
        let (w, h) = (area.w as u16, area.h as u16);
        let start = match mode {
            MirrorOrientation::Fit => VideoStart { box_width: w, box_height: h, swap: false, quality: 0, max_fps: 0, strips: STRIPS },
            MirrorOrientation::Device | MirrorOrientation::Rotate => {
                let (w, h) = if frame.rotation.swaps_axes() { (h, w) } else { (w, h) };
                VideoStart { box_width: w, box_height: h, swap: true, quality: 0, max_fps: 0, strips: STRIPS }
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
        let strip = match video.next_frame(|| shared.spare.lock().pop().unwrap_or_else(|| Vec::new_in(Caps::SPIRAM))).await {
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
        if let Some(old) = old {
            shared.recycle(old);
        }
        shared.wake.give();
    }
}

struct Player {
    surface: ExternalSurface,
    shared: Arc<Shared>,
    link: Link,
    mode: MirrorOrientation,
    decoder: Option<(ImageFormat, Decoder)>,
    shown: Vec<Frame<Caps>>,
    geometry: Option<Geometry>,
    session: Option<JoinHandle<()>>,
    blacked: [Option<Rect>; MAX_FRAME_BUFFERS],
    /// Per framebuffer, the `seq` of the frame it holds whole.
    held: [u32; MAX_FRAME_BUFFERS],
    /// Per framebuffer, what to write again.
    stale: [Rect; MAX_FRAME_BUFFERS],
    seq: u32,
    stats: (Instant, u32, usize),
}

impl Player {
    fn run(&mut self, sram: &mut SharedSram) {
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
            let geometry = Geometry::of(&frame, self.mode);
            let reopen = self.geometry.is_none_or(|g| g.start != geometry.start || g.device_rotation(self.mode) != geometry.device_rotation(self.mode));
            if self.geometry != Some(geometry) {
                self.geometry = Some(geometry);
                self.blacked = [None; MAX_FRAME_BUFFERS];
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

    fn render(&mut self, frame: &ExternalFrame, strip: &Frame<Caps>, (rect, turn): (Rect, Rotation), sram: &mut SharedSram) -> idf::Result<()> {
        let mut transform = Transform::new();
        transform.rotation = turn;
        let at = strip_rect(&strip.head, rect, turn);
        transform.at = (at.x, at.y);
        frame.wrote(at);
        let mut target = frame.target().ok_or(idf::Error::INVALID_ARG)?;
        let decoder = decoder_for(&mut self.decoder, frame.fb.format())?;
        let bytes = sram.as_mut();
        let (a, b) = bytes.split_at_mut(bytes.len() / 2);
        decoder.render(&strip.jpeg, &mut [a, b], &transform, &mut target)?.wait()?;
        self.stats.2 += strip.jpeg.len();
        Ok(())
    }

    /// Draws strips as they arrive; `false` when the frame did not come
    /// whole.
    fn draw_new(&mut self, frame: &ExternalFrame, first: Frame<Caps>, sram: &mut SharedSram) -> idf::Result<bool> {
        let mut drawn = Vec::new();
        let complete = self.draw_strips(frame, first, &mut drawn, sram);
        if matches!(complete, Ok(true)) {
            drawn = core::mem::replace(&mut self.shown, drawn);
            self.seq = self.seq.wrapping_add(1);
            self.held[buffer(frame)] = self.seq;
        }
        drawn.into_iter().for_each(|s| self.shared.recycle(s));
        complete
    }

    fn draw_strips(&mut self, frame: &ExternalFrame, first: Frame<Caps>, drawn: &mut Vec<Frame<Caps>>, sram: &mut SharedSram) -> idf::Result<bool> {
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

    fn next_strip(&self) -> Option<Frame<Caps>> {
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

    fn redraw(&mut self, frame: &ExternalFrame, sram: &mut SharedSram) -> idf::Result<bool> {
        let shown = core::mem::take(&mut self.shown);
        let result = (|| {
            let Some(first) = shown.first() else { return Ok(false) };
            let index = buffer(frame);
            let whole = self.held[index] != self.seq;
            let stale = self.stale[index];
            let Some((rect, turn, moved)) = self.place(frame, &first.head)? else { return Ok(false) };
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
    swipe: Option<(i16, i16)>,
    /// The surface is in the view; screen moves cannot draw over it.
    live: bool,
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
            wake: Semaphore::new(0, 1),
            strips: Mutex::new(VecDeque::new()),
            spare: Mutex::new(Vec::new()),
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
        Some(Self { link, mode: settings::mirror_orientation(), surface, shared, player: None, switching: false, leaving: false, bar: true, passthrough: true, swipe: None, live: false })
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

    fn play(&mut self, mut sram: Sram) {
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
        };
        let spawned = thread::Builder::new().name("mirror").stack_size(8192).priority(5).spawn(move || {
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

    fn restore(&mut self, sram: Sram, cx: &mut Cx<Self>) {
        cx.set_render_mode(RenderMode { strips: Strips::new(sram), lanes: STRIP_LANES, display: Default::default() }, MirrorMsg::Restored);
    }

    fn show_bar(&mut self, shown: bool) {
        self.bar = shown;
        self.shared.bar.store(shown, Ordering::Release);
        *self.shared.refresh.lock() = Some(Rect::new(0, 0, i16::MAX, i16::MAX));
        self.shared.wake.give();
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
                    self.play(sram);
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
            let bar = control_bar(ui, w < h, self.passthrough).absolute(Some(BAR_MARGIN), None, None, Some(BAR_MARGIN));
            children.push(if self.live { bar.appear(presence) } else { bar }.id(id!("controls")));
        }
        ui.column(children).background(BLACK)
    }
}

fn control_bar<'a>(ui: &'a Ui<MirrorMsg>, vertical: bool, passthrough: bool) -> Element<'a, MirrorMsg> {
    let mut groups: [&[(char, Option<MirrorMsg>)]; 4] = [
        &[(icons::HIDE, Some(MirrorMsg::Hide))],
        &[(icons::NAV_BACK, None), (icons::NAV_HOME, None), (icons::NAV_RECENTS, None)],
        &[(icons::VOLUME_DOWN, None), (icons::VOLUME_UP, None), (icons::POWER, None)],
        &[(icons::TOUCH, Some(MirrorMsg::Passthrough)), (icons::DISPLAY_MODE, None), (icons::CLOSE, Some(MirrorMsg::Back))],
    ];
    if vertical {
        groups.reverse();
    }
    let mut items = Vec::new();
    for (i, group) in groups.iter().enumerate() {
        if i > 0 {
            items.push(if vertical { divider(ui).padding_x(ui.theme().widgets.unit.gap) } else { vertical_divider(ui) });
        }
        let mut keys: Vec<_> = group.iter().map(|&(glyph, msg)| control_key(ui, glyph, msg, passthrough)).collect();
        if vertical {
            keys.reverse();
        }
        items.extend(keys);
    }
    let pad = (BAR_THICKNESS - KEY_SIZE) / 2;
    let bar = if vertical { ui.column(items).width(BAR_THICKNESS).padding_y(pad) } else { ui.row(items).height(BAR_THICKNESS).padding_x(pad) };
    bar.align(Align::Center).radius(BAR_RADIUS).background(ui.colors().surface)
}

fn control_key<'a>(ui: &'a Ui<MirrorMsg>, glyph: char, msg: Option<MirrorMsg>, passthrough: bool) -> Element<'a, MirrorMsg> {
    let color = match msg {
        Some(MirrorMsg::Back) => colors::DESTRUCTIVE,
        Some(MirrorMsg::Passthrough) if passthrough => ColorRole::Primary,
        _ => ColorRole::Text,
    };
    let key = ui
        .row([ui.icon(icon(glyph)).foreground(color)])
        .size(KEY_SIZE, KEY_SIZE)
        .radius(14)
        .align(Align::Center)
        .justify(Justify::Center)
        .when(State::PRESSED, &[Prop::Background(colors::PRESSED)]);
    match msg {
        Some(msg) => key.on_tap(msg),
        None => key,
    }
}

impl Screen for MirrorScreen {
    fn style(&self) -> ScreenStyle {
        ScreenStyle { bars: Bars::Hidden, ..ScreenStyle::default() }
    }

    fn did_appear(&mut self, cx: &mut Cx<Self>) {
        self.leaving = false;
        self.live = true;
        cx.rebuild();
        if !self.switching && self.player.is_none() {
            self.start(cx);
        }
    }

    fn will_disappear(&mut self, cx: &mut Cx<Self>) {
        self.live = false;
        cx.rebuild();
        if self.switching {
            self.leaving = true;
            return;
        }
        if let Some(sram) = self.stop() {
            self.release_rotation();
            self.restore(sram, cx);
        }
    }
}
