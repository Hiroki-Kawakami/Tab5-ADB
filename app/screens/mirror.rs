/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::sync::Arc;
use alloc::vec::Vec;
use core::ffi::CStr;
use core::sync::atomic::{AtomicBool, Ordering};
use core::time::Duration;

use agent_link::protocol::ROTATION_RELEASE;
use agent_link::{Frame, Jpeg, Link, VideoStart};
use bsp_rs::display;
use devkit::Lease;
use devkit_gfx::{Color, ImageFormat, Rect, Rotation, Target};
use devkit_ui::{Bars, Component, Cx, Element, ExternalFrame, ExternalSurface, Policy, RenderMode, Screen, ScreenStyle, Strips, Ui, id};
use idf::executor::JoinHandle;
use idf::cache::{self, Flags};
use idf::heap::Caps;
use idf::log::{error, info};
use idf::sync::{Mutex, Semaphore};
use idf::thread;
use idf::time::Instant;
use media_engine::{DecodeConfig, Decoder, Transform, Yuv, YuvRange, YuvStandard};

use crate::adb_link;
use crate::settings::{self, MirrorOrientation};
use crate::{STRIP_LANES, SharedSram};

const TAG: &CStr = c"mirror";
const UI_STRIP_BYTES: usize = 128 * 1024;
const MIRROR_FRAME_BUFFERS: u8 = 3;
const UI_FRAME_BUFFERS: u8 = 2;
const OPEN_TIMEOUT: Duration = Duration::from_secs(5);
const ROTATION_TIMEOUT: Duration = Duration::from_secs(2);
const IDLE: Duration = Duration::from_millis(20);
const STATS_PERIOD: Duration = Duration::from_secs(1);
const MAX_FRAME_BUFFERS: usize = 4;
const BLACK: Color = Color::rgb(0, 0, 0);

type Spare = Vec<u8, Caps>;
type Sram = Lease<'static, SharedSram>;

struct Shared {
    stop: AtomicBool,
    refresh: AtomicBool,
    wake: Semaphore,
    latest: Mutex<Option<Frame<Caps>>>,
    spare: Mutex<Vec<Spare>>,
}

impl Shared {
    fn recycle(&self, frame: Frame<Caps>) {
        if let Jpeg::Copied(vec) = frame.jpeg {
            self.spare.lock().push(vec);
        }
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
            MirrorOrientation::Fit => VideoStart { box_width: w, box_height: h, swap: false, quality: 0, max_fps: 0 },
            MirrorOrientation::Device | MirrorOrientation::Rotate => {
                let (w, h) = if frame.rotation.swaps_axes() { (h, w) } else { (w, h) };
                VideoStart { box_width: w, box_height: h, swap: true, quality: 0, max_fps: 0 }
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
        let frame = match video.next_frame(|| shared.spare.lock().pop().unwrap_or_else(|| Vec::new_in(Caps::SPIRAM))).await {
            Ok(frame) => frame,
            Err(err) => {
                error!(TAG, "video: {err}");
                return;
            }
        };
        let old = shared.latest.lock().replace(frame);
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
    shown: Option<Frame<Caps>>,
    geometry: Option<Geometry>,
    session: Option<JoinHandle<()>>,
    blacked: [Option<Rect>; MAX_FRAME_BUFFERS],
    stats: (Instant, u32, usize),
}

impl Player {
    fn run(&mut self, sram: &mut SharedSram) {
        while !self.shared.stop.load(Ordering::Acquire) {
            self.shared.wake.take_timeout(IDLE);
            let fresh = self.shared.latest.lock().take();
            let fresh = fresh.is_some_and(|received| {
                if let Some(old) = self.shown.replace(received) {
                    self.shared.recycle(old);
                }
                true
            });
            let refresh = self.shared.refresh.swap(false, Ordering::AcqRel);
            if refresh {
                self.blacked = [None; MAX_FRAME_BUFFERS];
            }
            if !fresh && !refresh && self.geometry.is_some() {
                continue;
            }
            let Some(frame) = self.surface.begin() else { continue };
            let geometry = Geometry::of(&frame, self.mode);
            let reopen = self.geometry.is_none_or(|g| g.start != geometry.start || g.device_rotation(self.mode) != geometry.device_rotation(self.mode));
            if self.geometry != Some(geometry) {
                self.geometry = Some(geometry);
                self.blacked = [None; MAX_FRAME_BUFFERS];
            }
            if reopen {
                self.restart(geometry);
            }
            match self.draw(&frame, sram) {
                Ok(true) => {
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

    /// `false` when there is nothing to show in this geometry yet: a frame
    /// sent for the box before the screen turned may not fit.
    fn draw(&mut self, frame: &ExternalFrame, sram: &mut SharedSram) -> idf::Result<bool> {
        let (Some(shown), Some(geometry)) = (self.shown.as_ref(), self.geometry) else { return Ok(false) };
        let area = frame.area;
        let turn = geometry.turn(self.mode, shown.head.rotation);
        let (w, h) = (shown.head.width as i16, shown.head.height as i16);
        let (w, h) = if turn.swaps_axes() { (h, w) } else { (w, h) };
        let rect = Rect::new(area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h);
        if !area.contains(&rect) {
            return Ok(false);
        }
        let mut target = frame.target().ok_or(idf::Error::INVALID_ARG)?;
        let index = frame.index.min(MAX_FRAME_BUFFERS - 1);
        if self.blacked[index] != Some(rect) {
            for margin in margins(area, rect).into_iter().filter(|r| !r.is_empty()) {
                clear(&mut target, margin)?;
            }
            self.blacked[index] = Some(rect);
        }
        let mut transform = Transform::new();
        transform.rotation = turn;
        transform.at = (rect.x, rect.y);
        let decoder = decoder_for(&mut self.decoder, frame.fb.format())?;
        let bytes = sram.as_mut();
        let (a, b) = bytes.split_at_mut(bytes.len() / 2);
        decoder.render(&shown.jpeg, &mut [a, b], &transform, &mut target)?.wait()?;
        self.stats.2 += shown.jpeg.len();
        Ok(true)
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
}

pub struct MirrorScreen {
    link: Link,
    mode: MirrorOrientation,
    surface: ExternalSurface,
    shared: Arc<Shared>,
    player: Option<thread::JoinHandle<Sram>>,
    switching: bool,
    leaving: bool,
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
            refresh: AtomicBool::new(false),
            wake: Semaphore::new(0, 1),
            latest: Mutex::new(None),
            spare: Mutex::new(Vec::new()),
        });
        let refresh = shared.clone();
        surface.set_on_refresh(move |_| {
            refresh.refresh.store(true, Ordering::Release);
            refresh.wake.give();
        });
        Some(Self { link, mode: settings::mirror_orientation(), surface, shared, player: None, switching: false, leaving: false })
    }

    fn start(&mut self, cx: &mut Cx<Self>) {
        let strips = match Strips::allocate(UI_STRIP_BYTES, Caps::SPIRAM) {
            Ok(strips) => strips,
            Err(err) => {
                error!(TAG, "strips: {err}");
                return;
            }
        };
        let mut display = display::Config::default();
        display.fb_num = MIRROR_FRAME_BUFFERS;
        cx.set_render_mode(RenderMode { strips, lanes: 1, display }, MirrorMsg::Started);
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
            shown: None,
            geometry: None,
            session: None,
            blacked: [None; MAX_FRAME_BUFFERS],
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
        Some(player.join())
    }

    fn restore(&mut self, sram: Sram, cx: &mut Cx<Self>) {
        let mut display = display::Config::default();
        display.fb_num = UI_FRAME_BUFFERS;
        cx.set_render_mode(RenderMode { strips: Strips::new(sram), lanes: STRIP_LANES, display }, MirrorMsg::Restored);
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
            MirrorMsg::Back => cx.nav().pop(),
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
        }
    }

    fn view<'a>(&'a self, ui: &'a Ui<MirrorMsg>) -> Element<'a, MirrorMsg> {
        ui.column([ui.external(&self.surface).grow(1).id(id!("mirror"))]).background(BLACK).on_tap(MirrorMsg::Back)
    }
}

impl Screen for MirrorScreen {
    fn style(&self) -> ScreenStyle {
        ScreenStyle { bars: Bars::Hidden, ..ScreenStyle::default() }
    }

    fn did_appear(&mut self, cx: &mut Cx<Self>) {
        self.leaving = false;
        if !self.switching && self.player.is_none() {
            self.start(cx);
        }
    }

    fn will_disappear(&mut self, cx: &mut Cx<Self>) {
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
