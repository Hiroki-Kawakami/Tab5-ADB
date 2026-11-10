/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::format;
use alloc::string::String;
use alloc::sync::Arc;
use alloc::vec;
use alloc::vec::Vec;
use core::ffi::CStr;
use core::sync::atomic::{AtomicBool, Ordering};

use adb::link::Connection;
use adb::services::sync::{DATA_MAX, Sync};
use devkit_gfx::{ImageBuf, ImageFormat, ImageSource, Size, Target};
use devkit_image::{DecodeOptions, Decoder, Error, Fit, FileStream, Format, Orientation, ResizeOptions, Stream};
use devkit_ui::widgets::{divider, icon_button, progress_indicator, table_header};
use devkit_ui::{Align, Canvas, Component, Corners, Cx, Edge, Elide, Element, Justify, Overlay, Presence, Screen, Task, This, Ui, id};
use idf::executor::{self, JoinHandle, Receiver, Sender, Spawner, oneshot};
use idf::heap::Caps;
use idf::log::error;
use idf::thread;
use media_engine::{Slot, Srm};

use crate::adb_link;
use crate::exif::{self, Exif};
use crate::files::{self, FileRef, Volume};
use crate::fonts::icon;
use crate::preview::Picture;
use crate::resources::{colors, icons};

use super::file_preview::{FilePreviewScreen, Msg, inline, stacked};

const TAG: &CStr = c"image_preview";
const RADIUS: u8 = 8;
const FORMAT: ImageFormat = ImageFormat::Rgb565;
const HEAD: usize = 128 * 1024;
const PIECES: usize = 4;
const STACK: usize = 16 * 1024;
const PRIORITY: u32 = 3;
/// Away from the UI task's core: it runs below `PRIORITY`.
const CORE: u32 = 1;
const MARGIN: i16 = 24;
const GAP: i16 = 16;
const SHEET_WIDTH: i16 = 880;
const SHEET_RADIUS: u8 = 24;
const SHEET_MAX_HEIGHT: i16 = 560;
const UNSUPPORTED: &str = "This image format is not supported.";
const UNREADABLE: &str = "The image could not be read.";
const UNDECODABLE: &str = "The image could not be decoded.";
const NO_MEMORY: &str = "Not enough memory to show this image.";

#[derive(Clone, Copy)]
pub enum ImageMsg {
    Area(i16, i16),
}

/// What the file says of the picture, upright.
pub struct Details {
    pub width: u16,
    pub height: u16,
    pub exif: Option<Exif>,
}

enum State {
    Loading,
    Failed(&'static str),
    Shown(Arc<Picture>, Arc<dyn ImageSource>),
}

fn shown(picture: Picture) -> State {
    let picture = Arc::new(picture);
    State::Shown(picture.clone(), picture)
}

/// The picture, contain-fit into the space below the top bar, decoded again
/// for each size that space takes. Until then the picture shown is rescaled to
/// stand in for it.
pub struct ImagePreview {
    state: State,
    area: Option<(u16, u16)>,
    loading: Option<(Task, Arc<AtomicBool>)>,
    rescaling: Option<Task>,
    details: Option<Details>,
}

struct Decoded {
    picture: Picture,
    details: Details,
}

impl ImagePreview {
    pub fn new() -> Self {
        Self { state: State::Loading, area: None, loading: None, rescaling: None, details: None }
    }

    pub fn details(&self) -> Option<&Details> {
        self.details.as_ref()
    }

    fn stop(&mut self) {
        if let Some((task, stop)) = self.loading.take() {
            stop.store(true, Ordering::Relaxed);
            task.abort();
        }
        if let Some(task) = self.rescaling.take() {
            task.abort();
        }
    }

    fn stand_in(&mut self, area: (u16, u16), cx: &mut Cx<FilePreviewScreen>) {
        let (State::Shown(picture, _), Some(details)) = (&self.state, &self.details) else {
            self.state = State::Loading;
            return;
        };
        let picture = picture.clone();
        let size = fit((details.width, details.height), area);
        self.rescaling = Some(cx.spawn(move |this: This<FilePreviewScreen>| async move {
            let result = rescale(&picture, size).await;
            drop(picture);
            this.with(|s, cx| {
                let Some(image) = s.image_mut() else { return };
                image.rescaling = None;
                match result {
                    Ok(stand_in) if image.loading.is_some() && image.area == Some(area) => image.state = shown(stand_in),
                    Ok(_) => {}
                    Err(err) => {
                        error!(TAG, "rescale: {err}");
                        if image.loading.is_some() {
                            image.state = State::Loading;
                        }
                    }
                }
                cx.rebuild();
            });
        }));
    }

    pub fn update(screen: &mut FilePreviewScreen, msg: ImageMsg, cx: &mut Cx<FilePreviewScreen>) {
        match msg {
            ImageMsg::Area(width, height) => {
                let file = screen.file().clone();
                let Some(image) = screen.image_mut() else { return };
                let area = (width.max(1) as u16, height.max(1) as u16);
                if image.area == Some(area) {
                    return;
                }
                image.area = Some(area);
                image.stop();
                image.stand_in(area, cx);
                let stop = Arc::new(AtomicBool::new(false));
                let flag = stop.clone();
                let task = cx.spawn(move |this: This<FilePreviewScreen>| async move {
                    let result = decode_on_worker(file, area, flag).await;
                    this.with(|s, cx| {
                        let Some(image) = s.image_mut() else { return };
                        image.loading = None;
                        match result {
                            Ok(decoded) => {
                                image.state = shown(decoded.picture);
                                image.details = Some(decoded.details);
                            }
                            Err(text) => image.state = State::Failed(text),
                        }
                        cx.rebuild();
                    });
                });
                image.loading = Some((task, stop));
            }
        }
    }

    pub fn view<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let probe = ui.canvas(draw_nothing).absolute(Some(0), Some(0), Some(0), Some(0)).on_size(|w, h| Msg::Image(ImageMsg::Area(w, h)));
        let content = match &self.state {
            State::Shown(_, source) => ui.column([ui.image(source)]).radius(RADIUS).clip(),
            State::Failed(text) => ui.text(ui.theme().fonts.body, text).foreground(colors::MUTED),
            State::Loading => progress_indicator(ui, None),
        };
        ui.column([probe, content]).grow(1).align_self(Align::Stretch).align(Align::Center).justify(Justify::Center).clip()
    }
}

/// Contain-fit, never enlarged, rounded as `resize_decoder` rounds.
fn fit((sw, sh): (u16, u16), (aw, ah): (u16, u16)) -> (u16, u16) {
    if sw <= aw && sh <= ah {
        return (sw, sh);
    }
    let (sw, sh, aw, ah) = (u32::from(sw), u32::from(sh), u32::from(aw), u32::from(ah));
    if aw * sh <= ah * sw { (aw as u16, ((sh * aw + sw / 2) / sw).max(1) as u16) } else { (((sw * ah + sh / 2) / sh).max(1) as u16, ah as u16) }
}

/// The 2D engine scales in sixteenths: the stand-in may fall short of `size`.
async fn rescale(picture: &Picture, (width, height): (u16, u16)) -> Result<Picture, String> {
    let (pw, ph) = (u32::from(picture.width()), u32::from(picture.height()));
    let sixteenths = (u32::from(width) * 16 / pw).min(u32::from(height) * 16 / ph).max(1);
    let (w, h) = ((pw * sixteenths / 16).max(1) as u16, (ph * sixteenths / 16).max(1) as u16);
    let ppa = media_engine::ppa().map_err(|err| format!("ppa: {err}"))?;
    let mut out = ImageBuf::new_in(FORMAT, w, h, usize::from(w), 64, Caps::SPIRAM).map_err(|_| format!("alloc {w}x{h}"))?;
    {
        let mut target = Target::new(out.pixels_mut(), FORMAT, Size::new(w, h), usize::from(w)).ok_or("target")?;
        let mut op = Srm::new(picture.image());
        op.scale_x = sixteenths as f32 / 16.0;
        op.scale_y = op.scale_x;
        let mut slot = Slot::new();
        ppa.srm(&op, &mut target, &mut slot).map_err(|err| format!("srm: {err}"))?.await.map_err(|err| format!("srm: {err}"))?;
    }
    Ok(out)
}

impl Drop for ImagePreview {
    fn drop(&mut self) {
        self.stop();
    }
}

fn draw_nothing<'a>(_: &'a FilePreviewScreen, _: &mut Canvas<'a, '_>) {}

async fn decode_on_worker(file: FileRef, area: (u16, u16), stop: Arc<AtomicBool>) -> Result<Decoded, &'static str> {
    let (tx, rx) = oneshot::channel();
    thread::Builder::new()
        .name("image")
        .stack_size(STACK)
        .stack_caps(devkit_image::internal_caps())
        .priority(PRIORITY)
        .core(Some(CORE))
        .spawn(move || drop(tx.send(decode_file(&file, area, &stop))))
        .map_err(|err| {
            error!(TAG, "thread: {err}");
            UNDECODABLE
        })?;
    rx.await.map_err(|_| UNDECODABLE)?
}

fn decode_file(file: &FileRef, area: (u16, u16), stop: &Arc<AtomicBool>) -> Result<Decoded, &'static str> {
    match &file.volume {
        Volume::Sd => {
            let path = files::c_path(&file.path).map_err(|_| UNREADABLE)?;
            let sd = idf::fs::File::open(&path).map_err(|err| {
                error!(TAG, "open {}: {err}", file.path);
                UNREADABLE
            })?;
            let head = read_head(|buf| sd.read(buf).map_err(|err| error!(TAG, "read {}: {err}", file.path)))?;
            decode(Local { stream: FileStream::new(&sd, 0, None), head, stop }, area)
        }
        Volume::Android(android) => {
            let Some(spawner) = adb_link::spawner() else {
                error!(TAG, "adb not started");
                return Err(UNREADABLE);
            };
            let mut remote = Remote { conn: android.connection().clone(), path: file.path.clone(), spawner, stop, head: Vec::new_in(Caps::SPIRAM), pos: 0, seekable: false, feed: None };
            let mut feed = remote.open(0);
            remote.head = read_head(|buf| feed.read(buf, stop).map_err(|_| ()))?;
            remote.feed = Some(feed);
            decode(remote, area)
        }
    }
}

fn read_head(mut read: impl FnMut(&mut [u8]) -> Result<usize, ()>) -> Result<Vec<u8, Caps>, &'static str> {
    let mut head = idf::heap::try_vec(HEAD, Caps::SPIRAM).map_err(|_| NO_MEMORY)?;
    head.resize(HEAD, 0);
    let mut filled = 0;
    while filled < HEAD {
        match read(&mut head[filled..]).map_err(|_| UNREADABLE)? {
            0 => break,
            n => filled += n,
        }
    }
    head.truncate(filled);
    Ok(head)
}

/// A stream that also lends its first bytes, read before the decoder opens.
trait Source: Stream {
    fn head(&self) -> &[u8];

    /// For a decoder that seeks, at whatever cost a seek has here.
    fn allow_seek(&mut self) {}
}

struct Local<'a> {
    stream: FileStream<'a>,
    head: Vec<u8, Caps>,
    stop: &'a AtomicBool,
}

impl Stream for Local<'_> {
    fn read(&mut self, dst: &mut [u8]) -> devkit_image::Result<usize> {
        if self.stop.load(Ordering::Relaxed) {
            return Err(Error::Io);
        }
        self.stream.read(dst)
    }

    fn seek(&mut self, pos: usize) -> devkit_image::Result<()> {
        self.stream.seek(pos)
    }

    fn can_seek(&self) -> bool {
        true
    }
}

impl Source for Local<'_> {
    fn head(&self) -> &[u8] {
        &self.head
    }
}

enum Piece {
    Data(Vec<u8, Caps>),
    End,
    Failed,
}

/// The file from offset `at` on, as the adb task receives it.
struct Feed {
    rx: Receiver<Piece>,
    at: usize,
    piece: Vec<u8, Caps>,
    used: usize,
    ended: bool,
    task: Option<JoinHandle<()>>,
}

impl Feed {
    fn read(&mut self, dst: &mut [u8], stop: &AtomicBool) -> devkit_image::Result<usize> {
        while self.used == self.piece.len() {
            if self.ended {
                return Ok(0);
            }
            if stop.load(Ordering::Relaxed) {
                return Err(Error::Io);
            }
            match executor::block_on(self.rx.recv()) {
                Piece::Data(data) => {
                    self.piece = data;
                    self.used = 0;
                }
                Piece::End => self.ended = true,
                Piece::Failed => return Err(Error::Io),
            }
        }
        let n = dst.len().min(self.piece.len() - self.used);
        dst[..n].copy_from_slice(&self.piece[self.used..self.used + n]);
        self.used += n;
        self.at += n;
        Ok(n)
    }
}

impl Drop for Feed {
    fn drop(&mut self) {
        if let Some(task) = self.task.take() {
            task.abort();
        }
    }
}

/// An Android file: sync: from the start, where it is read straight through;
/// a seek past the head reopens it with `tail -c`, which sync: cannot do.
struct Remote<'a> {
    conn: Connection,
    path: String,
    spawner: &'static Spawner,
    stop: &'a AtomicBool,
    head: Vec<u8, Caps>,
    pos: usize,
    seekable: bool,
    feed: Option<Feed>,
}

impl Remote<'_> {
    fn open(&self, at: usize) -> Feed {
        let (tx, rx) = executor::channel(PIECES);
        let (conn, path) = (self.conn.clone(), self.path.clone());
        let task = self.spawner.spawn(async move { feed(conn, path, at, tx).await });
        Feed { rx, at, piece: Vec::new_in(Caps::SPIRAM), used: 0, ended: false, task: Some(task) }
    }
}

impl Stream for Remote<'_> {
    fn read(&mut self, dst: &mut [u8]) -> devkit_image::Result<usize> {
        if self.pos < self.head.len() {
            let n = dst.len().min(self.head.len() - self.pos);
            dst[..n].copy_from_slice(&self.head[self.pos..self.pos + n]);
            self.pos += n;
            return Ok(n);
        }
        if self.feed.as_ref().is_none_or(|feed| feed.at != self.pos) {
            self.feed = None;
            self.feed = Some(self.open(self.pos));
        }
        let n = self.feed.as_mut().map_or(Ok(0), |feed| feed.read(dst, self.stop))?;
        self.pos += n;
        Ok(n)
    }

    fn seek(&mut self, pos: usize) -> devkit_image::Result<()> {
        if !self.seekable {
            return Err(Error::Unsupported);
        }
        self.pos = pos;
        Ok(())
    }

    fn can_seek(&self) -> bool {
        self.seekable
    }
}

impl Source for Remote<'_> {
    fn head(&self) -> &[u8] {
        &self.head
    }

    fn allow_seek(&mut self) {
        self.seekable = true;
    }
}

async fn feed(conn: Connection, path: String, at: usize, tx: Sender<Piece>) {
    let result = if at == 0 { feed_sync(&conn, &path, &tx).await } else { feed_exec(&conn, &path, at, &tx).await };
    let last = match result {
        Ok(()) => Piece::End,
        Err(err) => {
            error!(TAG, "read {path} at {at}: {err}");
            Piece::Failed
        }
    };
    tx.send(last).await;
}

fn piece(data: &[u8]) -> Result<Piece, String> {
    let mut piece = idf::heap::try_vec(data.len(), Caps::SPIRAM).map_err(|_| format!("alloc {} bytes", data.len()))?;
    piece.extend_from_slice(data);
    Ok(Piece::Data(piece))
}

async fn feed_sync(conn: &Connection, path: &str, tx: &Sender<Piece>) -> Result<(), String> {
    let mut sync = Sync::open(conn).await.map_err(|err| format!("sync: {err}"))?;
    let mut recv = sync.recv(path).await.map_err(|err| format!("recv: {err}"))?;
    while let Some(data) = recv.next().await.map_err(|err| format!("recv: {err}"))? {
        if tx.is_closed() {
            return Ok(());
        }
        tx.send(piece(data)?).await;
    }
    Ok(())
}

async fn feed_exec(conn: &Connection, path: &str, at: usize, tx: &Sender<Piece>) -> Result<(), String> {
    let command = format!("exec:tail -c +{} {} 2>/dev/null", at + 1, files::shell_quote(path));
    let mut stream = conn.open(&command).await.map_err(|err| format!("open: {err}"))?;
    loop {
        let data = stream.read_slice(DATA_MAX).await.map_err(|err| format!("read: {err}"))?;
        if data.is_empty() {
            return Ok(());
        }
        if tx.is_closed() {
            return Ok(());
        }
        tx.send(piece(&data)?).await;
    }
}

fn read_at(stream: &mut impl Stream, at: usize, len: usize) -> Option<Vec<u8>> {
    stream.seek(at).ok()?;
    let mut out = alloc::vec![0; len];
    let mut filled = 0;
    while filled < len {
        match stream.read(&mut out[filled..]).ok()? {
            0 => break,
            n => filled += n,
        }
    }
    out.truncate(filled);
    Some(out)
}

fn orientation(tag: u8) -> Orientation {
    match tag {
        2 => Orientation::MirrorX,
        3 => Orientation::Rot180,
        4 => Orientation::MirrorY,
        5 => Orientation::Transpose,
        6 => Orientation::Rot90,
        7 => Orientation::Transverse,
        8 => Orientation::Rot270,
        _ => Orientation::None,
    }
}

fn failure(stage: &str, err: Error) -> &'static str {
    error!(TAG, "{stage}: {err}");
    match err {
        Error::OutOfMemory | Error::TooLarge => NO_MEMORY,
        Error::Unsupported => UNSUPPORTED,
        Error::Io => UNREADABLE,
        _ => UNDECODABLE,
    }
}

fn decode(mut src: impl Source, (width, height): (u16, u16)) -> Result<Decoded, &'static str> {
    let Some(format) = devkit_image::sniff(src.head()) else { return Err(UNSUPPORTED) };
    if matches!(format, Format::Tiff | Format::Webp) {
        src.allow_seek();
    }
    let mut exif = exif::parse(src.head());
    if exif.is_none() && exif::webp_has_exif(src.head()) {
        let start: Vec<u8> = src.head()[..21].to_vec();
        exif = exif::webp_exif(&start, |at, len| read_at(&mut src, at, len));
        src.seek(0).map_err(|err| failure("rewind", err))?;
    }
    let orientation = exif.as_ref().map_or(Orientation::None, |exif| orientation(exif.orientation));
    let swap = orientation.swaps_axes();
    let (tw, th) = if swap { (height, width) } else { (width, height) };
    let options = DecodeOptions { format: Some(FORMAT), target_width: tw, target_height: th, ..Default::default() };
    let mut decoder = Decoder::open(format, src, &options).map_err(|err| failure("open", err))?;
    let (sw, sh) = decoder.source_size();
    let (sw, sh) = if swap { (sh, sw) } else { (sw, sh) };
    let fits = sw <= width && sh <= height;
    let resize = ResizeOptions { target_width: if fits { 0 } else { width }, target_height: if fits { 0 } else { height }, fit: Fit::Contain, format: Some(FORMAT), caps: None };
    let picture = devkit_image::resize_decoder(&mut decoder, &resize, orientation).map_err(|err| failure("decode", err))?;
    Ok(Decoded { picture, details: Details { width: sw, height: sh, exif } })
}

#[derive(Clone, Copy)]
pub enum SheetMsg {
    Close,
}

/// A file's details and its picture's tags, in a sheet over the preview.
pub struct ImageInfoSheet {
    name: String,
    rows: Vec<(&'static str, String)>,
    location: String,
    camera: Vec<(&'static str, String)>,
    exposure: Vec<(&'static str, String)>,
}

impl ImageInfoSheet {
    pub fn new(name: String, mut rows: Vec<(&'static str, String)>, location: String, details: Option<&Details>) -> Self {
        let (mut camera, mut exposure) = (Vec::new(), Vec::new());
        if let Some(details) = details {
            rows.push(("Dimensions", format!("{} x {}", details.width, details.height)));
            if let Some(exif) = &details.exif {
                (camera, exposure) = (camera_rows(exif), exposure_rows(exif));
            }
        }
        Self { name, rows, location, camera, exposure }
    }
}

fn camera_rows(exif: &Exif) -> Vec<(&'static str, String)> {
    let mut rows = Vec::new();
    let camera = if exif.make.is_empty() || exif.model.is_empty() || exif.model.to_ascii_lowercase().starts_with(&exif.make.to_ascii_lowercase()) {
        if exif.model.is_empty() { exif.make.clone() } else { exif.model.clone() }
    } else {
        format!("{} {}", exif.make, exif.model)
    };
    if !camera.is_empty() {
        rows.push(("Camera", camera));
    }
    if !exif.lens.is_empty() {
        rows.push(("Lens", exif.lens.clone()));
    }
    if !exif.taken.is_empty() {
        rows.push(("Date Taken", taken(&exif.taken)));
    }
    if !exif.software.is_empty() {
        rows.push(("Software", exif.software.clone()));
    }
    rows
}

/// "YYYY:MM:DD HH:MM:SS", as EXIF stores it, with dashes in the date.
fn taken(stored: &str) -> String {
    let mut out = String::from(stored);
    let b = stored.as_bytes();
    if b.len() >= 10 && b[4] == b':' && b[7] == b':' {
        out.replace_range(4..5, "-");
        out.replace_range(7..8, "-");
    }
    out
}

fn mm(value: f32) -> String {
    if value == (value as i32) as f32 { format!("{} mm", value as i32) } else { format!("{value:.1} mm") }
}

fn exposure_rows(exif: &Exif) -> Vec<(&'static str, String)> {
    let mut rows = Vec::new();
    if let Some((num, den)) = exif.shutter {
        let seconds = f64::from(num) / f64::from(den);
        let text = if seconds >= 1.0 {
            if seconds == (seconds as u64) as f64 { format!("{} s", seconds as u64) } else { format!("{seconds:.1} s") }
        } else if num != 0 {
            format!("1/{:.0} s", f64::from(den) / f64::from(num))
        } else {
            String::from("0 s")
        };
        rows.push(("Shutter", text));
    }
    if let Some(aperture) = exif.aperture.filter(|&a| a > 0.0) {
        rows.push(("Aperture", format!("f/{aperture:.1}")));
    }
    if let Some(iso) = exif.iso {
        rows.push(("ISO", format!("{iso}")));
    }
    if let Some(focal) = exif.focal_mm.filter(|&f| f > 0.0) {
        rows.push(("Focal Length", mm(focal)));
    }
    if let Some(focal35) = exif.focal35_mm.filter(|&f| f > 0) {
        rows.push(("35mm Equiv.", format!("{focal35} mm")));
    }
    if let Some(bias) = exif.exposure_bias {
        rows.push(("Exposure Bias", if bias == 0.0 { String::from("0 EV") } else { format!("{bias:+.1} EV") }));
    }
    if let Some(flash) = exif.flash {
        rows.push(("Flash", String::from(if flash & 1 != 0 { "Fired" } else { "Did not fire" })));
    }
    rows
}

impl Component for ImageInfoSheet {
    type Msg = SheetMsg;

    fn update(&mut self, msg: SheetMsg, cx: &mut Cx<Self>) {
        match msg {
            SheetMsg::Close => cx.nav_of::<Overlay>().pop(),
        }
    }

    fn view<'a>(&'a self, ui: &'a Ui<SheetMsg>) -> Element<'a, SheetMsg> {
        let theme = ui.theme();
        let (width, height) = ui.screen_size();
        let insets = ui.insets();
        let head = ui.row([ui.text(theme.fonts.title, "Details").ellipsis(Elide::End).grow(1), icon_button(ui, icon(icons::CLOSE), SheetMsg::Close).id(id!("close"))]).gap(GAP).align(Align::Center);
        let header = |title: &'a str| table_header(ui, title).padding_x(0);
        let mut body = vec![header("File"), stacked(ui, "Name", &self.name, theme.fonts.body)];
        for (label, value) in &self.rows {
            body.extend([divider(ui), inline(ui, label, value)]);
        }
        body.extend([divider(ui), stacked(ui, "Location", &self.location, theme.fonts.caption)]);
        for (title, rows) in [("Camera", &self.camera), ("Exposure", &self.exposure)] {
            for (i, (label, value)) in rows.iter().enumerate() {
                body.push(if i == 0 { header(title) } else { divider(ui) });
                body.push(inline(ui, label, value));
            }
        }
        let sheet_width = if width > height { SHEET_WIDTH.min(width) } else { width };
        let sheet = ui
            .column([head, ui.scroll(ui.column(body)).max_height(SHEET_MAX_HEIGHT.min(height - MARGIN * 4))])
            .gap(8)
            .width(sheet_width)
            .padding_each(MARGIN, MARGIN, MARGIN, MARGIN + insets.bottom)
            .background(ui.colors().surface)
            .radius(SHEET_RADIUS)
            .corners(Corners::TOP)
            .appear(Presence::from_edge(Edge::Bottom))
            .disappear(Presence::from_edge(Edge::Bottom))
            .id(id!("sheet"));
        ui.column([sheet]).align(Align::Center).justify(Justify::End).absolute(Some(0), Some(0), Some(0), Some(0))
    }
}

impl Screen for ImageInfoSheet {
    fn scrim_tap(&self) -> Option<SheetMsg> {
        Some(SheetMsg::Close)
    }
}
