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

use agent_link::{Frame, Link, VideoStart};
use devkit_gfx::{ImageBuf, ImageFormat, ImageSource, Size, Target};
use devkit_ui::{Align, Child, Component, Cx, Element, Justify, This, Ui};
use idf::executor::{self, Sender};
use idf::heap::Caps;
use idf::log::error;
use media_engine::{DecodeConfig, Decoder, Header, Yuv, YuvRange, YuvStandard};

use crate::{adb_link, agent};

const TAG: &CStr = c"preview";
const OPEN_TIMEOUT: Duration = Duration::from_secs(5);
const RETRY: Duration = Duration::from_millis(500);
const QUALITY: u8 = 60;
const MAX_FPS: u8 = 10;
const BUFFERS: usize = 3;
const FORMAT: ImageFormat = ImageFormat::Rgb565;

type Picture = ImageBuf<Caps>;

/// The device's screen, contain-fit into a box of the props' size.
pub struct Preview {
    size: (u16, u16),
    image: Option<Arc<dyn ImageSource>>,
}

impl Child for Preview {
    fn create(size: &(u16, u16)) -> Self {
        Self { size: *size, image: None }
    }

    fn set_props(&mut self, size: &(u16, u16)) {
        self.size = *size;
    }
}

impl Component for Preview {
    type Msg = ();
    type Props = (u16, u16);

    fn update(&mut self, _: (), _: &mut Cx<Self>) {}

    fn view<'a>(&'a self, ui: &'a Ui<()>) -> Element<'a, ()> {
        let size = self.size;
        let image = match &self.image {
            Some(image) => ui.image(image),
            None => ui.empty(),
        };
        ui.column([image]).grow(1).align(Align::Center).justify(Justify::Center).task(move |this| show(this, size))
    }
}

async fn show(this: This<Preview>, size: (u16, u16)) {
    let yuv = Yuv { range: YuvRange::Full, standard: YuvStandard::Bt601 };
    let mut decoder = match Decoder::new(&DecodeConfig { format: FORMAT, yuv }) {
        Ok(decoder) => decoder,
        Err(err) => {
            error!(TAG, "decoder: {err}");
            return;
        }
    };
    let (stride, rows) = (size.0.next_multiple_of(16), size.1.next_multiple_of(16));
    let mut pool: Vec<Arc<Picture>> = Vec::new();
    loop {
        let Some(link) = agent::link() else {
            executor::sleep(RETRY).await;
            continue;
        };
        let Some(spawner) = adb_link::spawner() else {
            error!(TAG, "adb not started");
            return;
        };
        let (tx, mut rx) = executor::channel(1);
        let receiving = spawner.spawn(async move {
            if let Err(err) = receive(link, size, &tx).await {
                error!(TAG, "{err}");
            }
            tx.send(None).await;
        });
        let _receiving = devkit::guard(receiving, |task| task.abort());
        while let Some(frame) = rx.recv().await {
            let picture = match decode(&mut decoder, &frame.jpeg, &mut pool, (stride, rows)).await {
                Ok(Some(picture)) => picture,
                Ok(None) => continue,
                Err(err) => {
                    error!(TAG, "{err}");
                    continue;
                }
            };
            drop(frame);
            let picture: Arc<dyn ImageSource> = picture;
            let shown = this.with(|s, cx| {
                s.image = Some(picture);
                cx.rebuild();
            });
            if shown.is_none() {
                return;
            }
        }
        executor::sleep(RETRY).await;
    }
}

async fn receive(link: Link, (width, height): (u16, u16), tx: &Sender<Option<Frame<Caps>>>) -> Result<(), String> {
    let start = VideoStart { box_width: width, box_height: height, swap: false, quality: QUALITY, max_fps: MAX_FPS, strips: 1 };
    let mut video = link.open_video(&start, OPEN_TIMEOUT).await.map_err(|e| format!("open video: {e}"))?;
    while !tx.is_closed() {
        let frame = video.next_frame(|| Vec::new_in(Caps::SPIRAM)).await.map_err(|e| format!("video: {e}"))?;
        tx.send(Some(frame)).await;
    }
    Ok(())
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
