/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::format;
use alloc::string::String;
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::ffi::CStr;
use core::time::Duration;

use adb::link::Connection;
use agent_link::protocol::CAP_VIDEO;
use agent_link::{Frame, Link, VideoStart};
use devkit_gfx::{ImageBuf, ImageFormat, ImageSource, Size, Target};
use devkit_image::{DecodeOptions, Fit, Format, Orientation, ResizeOptions, SliceStream};
use devkit_ui::{Align, Child, Component, Cx, Element, Justify, This, Ui};
use idf::executor::{self, Sender, Spawner, oneshot};
use idf::heap::Caps;
use idf::log::{error, info};
use idf::thread;
use media_engine::{DecodeConfig, Decoder, Header, Yuv, YuvRange, YuvStandard};

use crate::{adb_link, agent};

const TAG: &CStr = c"preview";
const OPEN_TIMEOUT: Duration = Duration::from_secs(5);
const FIRST_FRAME_TIMEOUT: Duration = Duration::from_secs(5);
const RETRY: Duration = Duration::from_millis(500);
const QUALITY: u8 = 60;
const MAX_FPS: u8 = 10;
const BUFFERS: usize = 3;
const FORMAT: ImageFormat = ImageFormat::Rgb565;
const SCREENCAP_INTERVAL: Duration = Duration::from_secs(2);
const SCREENCAP_CHUNK: usize = 64 * 1024;
/// Room for a screenshot up front: growing the buffer copies it within PSRAM
/// fast enough to starve the display's reads and flash the panel blue.
const SCREENCAP_RESERVE: usize = 4 * 1024 * 1024;
const DECODE_STACK: usize = 16 * 1024;
const DECODE_PRIORITY: u32 = 3;
/// Away from the UI task's core: it runs below `DECODE_PRIORITY`.
const DECODE_CORE: u32 = 1;

type Picture = ImageBuf<Caps>;

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Props {
    pub width: u16,
    pub height: u16,
    /// Screenshots instead of the agent's video.
    pub screencap: bool,
}

/// The agent is up but its video cannot be used; the preview went on with
/// screenshots.
#[derive(Clone, Copy)]
pub struct VideoUnavailable;

/// The device's screen, contain-fit into a box of the props' size.
pub struct Preview {
    props: Props,
    image: Option<Arc<dyn ImageSource>>,
}

impl Child for Preview {
    fn create(props: &Props) -> Self {
        Self { props: *props, image: None }
    }

    fn set_props(&mut self, props: &Props) {
        self.props = *props;
    }
}

impl Component for Preview {
    type Msg = ();
    type Props = Props;
    type Event = VideoUnavailable;

    fn update(&mut self, _: (), _: &mut Cx<Self>) {}

    fn view<'a>(&'a self, ui: &'a Ui<()>) -> Element<'a, ()> {
        let props = self.props;
        let image = match &self.image {
            Some(image) => ui.image(image),
            None => ui.empty(),
        };
        ui.column([image]).grow(1).align(Align::Center).justify(Justify::Center).task(move |this| show(this, props))
    }
}

enum Outcome {
    Gone,
    Unavailable,
    Closed,
}

enum Received {
    Frame(Frame<Caps>),
    Unavailable,
    Ended,
}

async fn show(this: This<Preview>, props: Props) {
    let size = (props.width, props.height);
    let Some(spawner) = adb_link::spawner() else {
        error!(TAG, "adb not started");
        return;
    };
    if !props.screencap
        && let Some(link) = agent::link()
    {
        let outcome = if link.info().capabilities & CAP_VIDEO == 0 {
            error!(TAG, "agent has no video");
            Outcome::Unavailable
        } else {
            video(&this, spawner, link, size).await
        };
        match outcome {
            Outcome::Unavailable => {
                if this.with(|_, cx| cx.emit(VideoUnavailable)).is_none() {
                    return;
                }
            }
            Outcome::Gone | Outcome::Closed => return,
        }
    }
    screencap(&this, spawner, size).await;
}

/// `false` once the component has gone.
fn present(this: &This<Preview>, picture: Arc<dyn ImageSource>) -> bool {
    this.with(|s, cx| {
        s.image = Some(picture);
        cx.rebuild();
    })
    .is_some()
}

async fn video(this: &This<Preview>, spawner: &Spawner, link: Link, size: (u16, u16)) -> Outcome {
    let yuv = Yuv { range: YuvRange::Full, standard: YuvStandard::Bt601 };
    let mut decoder = match Decoder::new(&DecodeConfig { format: FORMAT, yuv }) {
        Ok(decoder) => decoder,
        Err(err) => {
            error!(TAG, "decoder: {err}");
            return Outcome::Unavailable;
        }
    };
    let (stride, rows) = (size.0.next_multiple_of(16), size.1.next_multiple_of(16));
    let mut pool: Vec<Arc<Picture>> = Vec::new();
    while !link.is_closed() {
        let (tx, mut rx) = executor::channel(1);
        let receiving = {
            let link = link.clone();
            spawner.spawn(async move {
                let end = receive(link, size, &tx).await;
                tx.send(end).await;
            })
        };
        let _receiving = devkit::guard(receiving, |task| task.abort());
        loop {
            let frame = match rx.recv().await {
                Received::Frame(frame) => frame,
                Received::Unavailable => return Outcome::Unavailable,
                Received::Ended => break,
            };
            let picture = match decode(&mut decoder, &frame.jpeg, &mut pool, (stride, rows)).await {
                Ok(Some(picture)) => picture,
                Ok(None) => continue,
                Err(err) => {
                    error!(TAG, "{err}");
                    continue;
                }
            };
            drop(frame);
            if !present(this, picture) {
                return Outcome::Gone;
            }
        }
        executor::sleep(RETRY).await;
    }
    Outcome::Closed
}

async fn receive(link: Link, (width, height): (u16, u16), tx: &Sender<Received>) -> Received {
    let start = VideoStart { box_width: width, box_height: height, swap: false, quality: QUALITY, max_fps: MAX_FPS, strips: 1, fill: false };
    let mut video = match link.open_video(&start, OPEN_TIMEOUT).await {
        Ok(video) => video,
        Err(err) => {
            error!(TAG, "open video: {err}");
            return Received::Unavailable;
        }
    };
    match executor::timeout(FIRST_FRAME_TIMEOUT, video.next_frame(|| Vec::new_in(Caps::SPIRAM))).await {
        Ok(Ok(frame)) => tx.send(Received::Frame(frame)).await,
        Ok(Err(err)) => {
            error!(TAG, "first frame: {err}");
            return Received::Unavailable;
        }
        Err(_) => {
            error!(TAG, "first frame: timed out");
            return Received::Unavailable;
        }
    }
    while !tx.is_closed() {
        match video.next_frame(|| Vec::new_in(Caps::SPIRAM)).await {
            Ok(frame) => tx.send(Received::Frame(frame)).await,
            Err(err) => {
                error!(TAG, "video: {err}");
                return Received::Ended;
            }
        }
    }
    Received::Ended
}

/// `None` while every buffer is still shown.
async fn decode(decoder: &mut Decoder, jpeg: &[u8], pool: &mut Vec<Arc<Picture>>, (stride, rows): (u16, u16)) -> Result<Option<Arc<Picture>>, String> {
    let header = Header::parse(jpeg).map_err(|e| format!("header: {e}"))?;
    let (w, h) = (header.padded_width, header.padded_height);
    if w > stride || h > rows {
        return Err(format!("frame {}x{} outside {stride}x{rows}", header.width, header.height));
    }
    let index = match pool.iter_mut().position(|p| Arc::get_mut(p).is_some()) {
        Some(index) => index,
        None if pool.len() < BUFFERS => {
            let picture = ImageBuf::new_in(FORMAT, stride, rows, usize::from(stride), 64, Caps::SPIRAM).map_err(|_| format!("alloc {stride}x{rows}"))?;
            pool.push(Arc::new(picture));
            pool.len() - 1
        }
        None => return Ok(None),
    };
    let picture = Arc::get_mut(&mut pool[index]).ok_or("buffer shared")?;
    picture.reshape(FORMAT, w, h, usize::from(w));
    let mut target = Target::new(picture.pixels_mut(), FORMAT, Size::new(w, h), usize::from(w)).ok_or("target")?;
    decoder.decode(jpeg, &mut target).map_err(|e| format!("decode: {e}"))?.await.map_err(|e| format!("decode: {e}"))?;
    picture.reshape(FORMAT, header.width, header.height, usize::from(w));
    Ok(Some(pool[index].clone()))
}

async fn screencap(this: &This<Preview>, spawner: &Spawner, size: (u16, u16)) {
    let Some(conn) = adb_link::connection() else {
        error!(TAG, "no connection");
        return;
    };
    let mut jpeg = true;
    loop {
        let shot = match capture(spawner, &conn, jpeg).await {
            Ok(shot) => shot,
            Err(err) => {
                error!(TAG, "screencap: {err}");
                executor::sleep(SCREENCAP_INTERVAL).await;
                continue;
            }
        };
        match devkit_image::sniff(&shot) {
            Some(Format::Jpeg) if jpeg => {}
            Some(Format::Png) if !jpeg => {}
            _ if jpeg => {
                info!(TAG, "screencap -j unsupported, using -p");
                jpeg = false;
                continue;
            }
            _ => {
                error!(TAG, "screencap -p: not a PNG");
                executor::sleep(SCREENCAP_INTERVAL).await;
                continue;
            }
        }
        match fit_on_worker(shot, size).await {
            Ok(picture) => {
                if !present(this, Arc::new(picture)) {
                    return;
                }
            }
            Err(err) => error!(TAG, "screencap: {err}"),
        }
        executor::sleep(SCREENCAP_INTERVAL).await;
    }
}

async fn capture(spawner: &Spawner, conn: &Connection, jpeg: bool) -> Result<Vec<u8, Caps>, String> {
    let conn = conn.clone();
    let mut task = devkit::guard(
        spawner.spawn(async move {
            let mut stream = conn.open(if jpeg { "exec:screencap -j" } else { "exec:screencap -p" }).await.map_err(|e| format!("open: {e}"))?;
            let mut shot = Vec::new_in(Caps::SPIRAM);
            shot.try_reserve(SCREENCAP_RESERVE).map_err(|_| format!("alloc {SCREENCAP_RESERVE} bytes"))?;
            loop {
                let slice = stream.read_slice(SCREENCAP_CHUNK).await.map_err(|e| format!("read: {e}"))?;
                if slice.is_empty() {
                    return Ok(shot);
                }
                shot.try_reserve(slice.len()).map_err(|_| format!("alloc {} bytes", shot.len() + slice.len()))?;
                shot.extend_from_slice(&slice);
            }
        }),
        |task| task.abort(),
    );
    (&mut *task).await
}

async fn fit_on_worker(shot: Vec<u8, Caps>, size: (u16, u16)) -> Result<Picture, String> {
    let (tx, rx) = oneshot::channel();
    thread::Builder::new()
        .name("screencap")
        .stack_size(DECODE_STACK)
        .stack_caps(devkit_image::internal_caps())
        .priority(DECODE_PRIORITY)
        .core(Some(DECODE_CORE))
        .spawn(move || drop(tx.send(fit(&shot, size))))
        .map_err(|e| format!("thread: {e}"))?;
    rx.await.map_err(|_| String::from("decoder gone"))?
}

fn fit(shot: &[u8], (width, height): (u16, u16)) -> Result<Picture, String> {
    let options = DecodeOptions { format: Some(FORMAT), target_width: width, target_height: height, ..Default::default() };
    let mut decoder = devkit_image::open(SliceStream::new(shot), &options).map_err(|e| format!("decode: {e}"))?;
    let resize = ResizeOptions { target_width: width, target_height: height, fit: Fit::Contain, format: Some(FORMAT), caps: None };
    devkit_image::resize_decoder(&mut decoder, &resize, Orientation::None).map_err(|e| format!("resize: {e}"))
}
