/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::sync::Arc;
use alloc::vec::Vec;
use core::future::poll_fn;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use core::task::{Poll, Waker};
use core::time::Duration;

use adb::link::{Connection, StreamReader, StreamWriter};
use idf::executor::{self, JoinHandle, Spawner, oneshot};
use idf::sync::Mutex;

use crate::protocol::{self, AppEntry, Rendered, RENDER_FILE, SCREENSHOT, SCREENSHOT_RELEASE, SCREENSHOT_RENDER, SCREENSHOT_SAVE, Channel, CAP_INPUT, HEADER_BYTES, ADAPT_DISPLAY, GET_APP_INFO, GET_MEDIA, GET_MEDIA_RENDER, HELLO, MEDIA_CONTROL, MediaAction, MediaRender, MediaState, Header, Hello, HelloReply, INPUT_CLEAR, KEY, OK, PROTO_VERSION, REPLY, SET_ROTATION, TOUCH, Touch, VideoStart};
use crate::audio::Audio;
use crate::protocol::{AudioCodec, AudioSource};
use crate::video::Video;
use crate::{Error, Result};

const HELLO_TIMEOUT: Duration = Duration::from_secs(3);

/// What the Tab5 takes, told to the agent in HELLO.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct HelloConfig {
    pub capabilities: u32,
    /// Control, video, audio.
    pub max_message: [u32; 3],
}

/// What the agent told in its HELLO reply.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct AgentInfo {
    pub version: [u8; 3],
    pub capabilities: u32,
    pub max_message: u32,
}

/// The newest event of one type, as new ones come.
pub struct EventWatch {
    shared: Arc<Shared>,
    kind: u8,
    seen: u64,
}

impl EventWatch {
    /// The newest event of its type not taken yet; an error once the link
    /// closes.
    pub async fn next(&mut self) -> Result<Vec<u8>> {
        poll_fn(|cx| {
            let mut events = self.shared.events.lock();
            if let Some((_, count, payload)) = events.latest.iter().find(|(k, count, _)| *k == self.kind && *count > self.seen) {
                self.seen = *count;
                return Poll::Ready(Ok(payload.clone()));
            }
            if self.shared.closed.load(Ordering::Acquire) {
                return Poll::Ready(Err(self.shared.error()));
            }
            if !events.waiters.iter().any(|w| w.will_wake(cx.waker())) {
                events.waiters.push(cx.waker().clone());
            }
            Poll::Pending
        })
        .await
    }
}

/// What GET_MEDIA_RENDER answered.
pub struct MediaRenderReply {
    payload: Vec<u8>,
}

impl MediaRenderReply {
    pub fn render(&self) -> Option<MediaRender<'_>> {
        MediaRender::parse(&self.payload)
    }
}

/// A RENDERED result.
pub struct RenderedReply {
    payload: Vec<u8>,
}

impl RenderedReply {
    pub fn rendered(&self) -> Rendered<'_> {
        Rendered::parse(&self.payload).unwrap_or(Rendered { width: 0, height: 0, jpeg: &[], attrs: Vec::new() })
    }
}

/// What GET_APP_INFO answered.
pub struct AppInfoReply {
    payload: Vec<u8>,
    count: usize,
}

impl AppInfoReply {
    /// One for each package asked, in order; `None` where the device has none.
    pub fn entries(&self) -> Vec<Option<AppEntry<'_>>> {
        protocol::app_entries(&self.payload, self.count).unwrap_or_default()
    }
}

struct Outbox {
    queued: Vec<u8>,
    waker: Option<Waker>,
}

struct Requests {
    last: u16,
    waiting: Vec<(u16, oneshot::Sender<Vec<u8>>)>,
}

struct Status {
    error: Option<Error>,
    waiters: Vec<Waker>,
}

struct Events {
    count: u64,
    /// The newest of each type: type, its number in `count`, payload.
    latest: Vec<(u8, u64, Vec<u8>)>,
    waiters: Vec<Waker>,
}

struct Shared {
    closed: AtomicBool,
    max_send: AtomicU32,
    outbox: Mutex<Outbox>,
    requests: Mutex<Requests>,
    status: Mutex<Status>,
    events: Mutex<Events>,
}

impl Shared {
    fn error(&self) -> Error {
        self.status.lock().error.clone().unwrap_or(Error::Closed)
    }

    fn shut(&self, error: Error) {
        if self.closed.swap(true, Ordering::AcqRel) {
            return;
        }
        let waiters = {
            let mut status = self.status.lock();
            status.error = Some(error);
            core::mem::take(&mut status.waiters)
        };
        waiters.into_iter().for_each(Waker::wake);
        drop(core::mem::take(&mut self.requests.lock().waiting));
        let waiters = core::mem::take(&mut self.events.lock().waiters);
        waiters.into_iter().for_each(Waker::wake);
        if let Some(waker) = self.outbox.lock().waker.take() {
            waker.wake();
        }
    }

    fn queue(&self, kind: u8, id: u16, payload: &[u8]) -> Result<()> {
        if payload.len() > self.max_send.load(Ordering::Acquire) as usize {
            return Err(Error::TooLarge);
        }
        let waker = {
            let mut outbox = self.outbox.lock();
            if self.closed.load(Ordering::Acquire) {
                return Err(self.error());
            }
            protocol::message(&mut outbox.queued, kind, id, payload);
            outbox.waker.take()
        };
        if let Some(waker) = waker {
            waker.wake();
        }
        Ok(())
    }

    fn register(&self) -> Result<(u16, oneshot::Receiver<Vec<u8>>)> {
        let mut requests = self.requests.lock();
        if self.closed.load(Ordering::Acquire) {
            return Err(self.error());
        }
        let mut id = requests.last;
        loop {
            id = id.wrapping_add(1);
            if id != 0 && requests.waiting.iter().all(|(w, _)| *w != id) {
                break;
            }
        }
        requests.last = id;
        let (tx, rx) = oneshot::channel();
        requests.waiting.push((id, tx));
        Ok((id, rx))
    }

    fn event(&self, kind: u8, payload: Vec<u8>) {
        let waiters = {
            let mut events = self.events.lock();
            events.count += 1;
            let count = events.count;
            match events.latest.iter_mut().find(|(k, _, _)| *k == kind) {
                Some(slot) => *slot = (kind, count, payload),
                None => events.latest.push((kind, count, payload)),
            }
            core::mem::take(&mut events.waiters)
        };
        waiters.into_iter().for_each(Waker::wake);
    }

    fn forget(&self, id: u16) {
        self.requests.lock().waiting.retain(|(w, _)| *w != id);
    }

    fn answer(&self, id: u16, payload: Vec<u8>) {
        let sender = {
            let mut requests = self.requests.lock();
            let at = requests.waiting.iter().position(|(w, _)| *w == id);
            at.map(|at| requests.waiting.swap_remove(at).1)
        };
        if let Some(sender) = sender {
            let _ = sender.send(payload);
        }
    }
}

struct Waiting<'a> {
    shared: &'a Shared,
    id: u16,
}

impl Drop for Waiting<'_> {
    fn drop(&mut self) {
        self.shared.forget(self.id);
    }
}

async fn read_loop(shared: Arc<Shared>, mut reader: StreamReader, max: u32) {
    let error = loop {
        let mut head = [0u8; HEADER_BYTES];
        if let Err(err) = reader.read_exact(&mut head).await {
            break err.into();
        }
        let header = Header::parse(&head);
        if header.length > max {
            break Error::Protocol;
        }
        let mut payload = alloc::vec![0u8; header.length as usize];
        if let Err(err) = reader.read_exact(&mut payload).await {
            break err.into();
        }
        if header.kind == REPLY {
            shared.answer(header.id, payload);
        } else if protocol::is_event(header.kind) {
            shared.event(header.kind, payload);
        }
    };
    shared.shut(error);
}

async fn write_loop(shared: Arc<Shared>, mut writer: StreamWriter) {
    let mut batch = Vec::new();
    loop {
        let more = poll_fn(|cx| {
            let mut outbox = shared.outbox.lock();
            if shared.closed.load(Ordering::Acquire) {
                return Poll::Ready(false);
            }
            if outbox.queued.is_empty() {
                outbox.waker = Some(cx.waker().clone());
                return Poll::Pending;
            }
            core::mem::swap(&mut outbox.queued, &mut batch);
            Poll::Ready(true)
        })
        .await;
        if !more {
            return;
        }
        if let Err(err) = writer.write(&batch).await {
            shared.shut(err.into());
            return;
        }
        batch.clear();
    }
}

struct Tasks {
    reader: Mutex<Option<JoinHandle<()>>>,
    writer: Mutex<Option<JoinHandle<()>>>,
}

impl Tasks {
    fn abort(&self) {
        for task in [self.reader.lock().take(), self.writer.lock().take()].into_iter().flatten() {
            task.abort();
        }
    }
}

impl Drop for Tasks {
    fn drop(&mut self) {
        self.abort();
    }
}

/// The control stream to one agent. Clones share it; the last one dropped
/// closes it, and the agent exits then.
#[derive(Clone)]
pub struct Link {
    shared: Arc<Shared>,
    tasks: Arc<Tasks>,
    info: AgentInfo,
    conn: Connection,
    id: Arc<str>,
    config: HelloConfig,
}

impl Link {
    /// Opens the control stream of the agent started with `id` and says
    /// HELLO; the link's tasks run on `spawner`.
    pub async fn open(conn: &Connection, spawner: &Spawner, id: &str, config: HelloConfig) -> Result<Self> {
        let stream = conn.open(&Channel::Control.service(id)).await?;
        let (reader, writer) = stream.into_split();
        let shared = Arc::new(Shared {
            closed: AtomicBool::new(false),
            max_send: AtomicU32::new(u32::MAX),
            outbox: Mutex::new(Outbox { queued: Vec::new(), waker: None }),
            requests: Mutex::new(Requests { last: 0, waiting: Vec::new() }),
            status: Mutex::new(Status { error: None, waiters: Vec::new() }),
            events: Mutex::new(Events { count: 0, latest: Vec::new(), waiters: Vec::new() }),
        });
        let tasks = Arc::new(Tasks {
            reader: Mutex::new(Some(spawner.spawn(read_loop(shared.clone(), reader, config.max_message[0])))),
            writer: Mutex::new(Some(spawner.spawn(write_loop(shared.clone(), writer)))),
        });
        let mut link = Self { shared, tasks, info: AgentInfo::default(), conn: conn.clone(), id: Arc::from(id), config };
        let hello = Hello { proto_version: PROTO_VERSION, capabilities: config.capabilities, max_message: config.max_message };
        let reply = link.request(HELLO, &hello.encode(), HELLO_TIMEOUT).await?;
        let reply = HelloReply::parse(&reply).ok_or(Error::Protocol)?;
        if reply.proto_version != PROTO_VERSION {
            return Err(Error::Protocol);
        }
        link.shared.max_send.store(reply.max_message, Ordering::Release);
        link.info = AgentInfo { version: reply.version, capabilities: reply.capabilities, max_message: reply.max_message };
        Ok(link)
    }

    pub fn info(&self) -> &AgentInfo {
        &self.info
    }

    /// Both ends gave every bit of `capabilities` in HELLO.
    pub fn supports(&self, capabilities: u32) -> bool {
        self.info.capabilities & self.config.capabilities & capabilities == capabilities
    }

    /// The result of a request answered `OK`; any other status is
    /// `Error::Status`. Requests run concurrently.
    pub async fn request(&self, kind: u8, args: &[u8], timeout: Duration) -> Result<Vec<u8>> {
        let shared = &*self.shared;
        let (id, answer) = shared.register()?;
        let _waiting = Waiting { shared, id };
        shared.queue(kind, id, args)?;
        let mut payload = match executor::timeout(timeout, answer).await {
            Ok(Ok(payload)) => payload,
            Ok(Err(_)) => return Err(shared.error()),
            Err(_) => return Err(Error::Timeout),
        };
        let (status, _) = protocol::reply(&payload).ok_or(Error::Protocol)?;
        if status != OK {
            return Err(Error::Status(status));
        }
        payload.drain(..protocol::REPLY_HEAD);
        Ok(payload)
    }

    /// Queues an input message; it goes out with whatever else is queued.
    pub fn input(&self, kind: u8, payload: &[u8]) -> Result<()> {
        if !self.supports(CAP_INPUT) {
            return Err(Error::Unsupported);
        }
        self.shared.queue(kind, 0, payload)
    }

    pub fn touch(&self, touch: &Touch) -> Result<()> {
        self.input(TOUCH, &touch.encode())
    }

    /// `keycode` is Android's `KeyEvent.KEYCODE_*`.
    pub fn key(&self, keycode: u16, down: bool) -> Result<()> {
        self.input(KEY, &protocol::key(keycode, down))
    }

    /// Has the agent let go of every finger and key still down.
    pub fn input_clear(&self) -> Result<()> {
        self.input(INPUT_CLEAR, &[])
    }

    /// A video stream sending as `start` asks; one at a time, so opening
    /// another ends the last.
    pub async fn open_video(&self, start: &VideoStart, timeout: Duration) -> Result<Video> {
        if self.is_closed() {
            return Err(self.shared.error());
        }
        Video::open(&self.conn, &Channel::Video.service(&self.id), start, self.config.max_message[1], timeout).await
    }

    /// The device's sound from `source`, which it stops playing itself while
    /// the stream is open; one at a time, so opening another ends the last.
    pub async fn open_audio(&self, codec: AudioCodec, source: AudioSource, timeout: Duration) -> Result<Audio> {
        if self.is_closed() {
            return Err(self.shared.error());
        }
        Audio::open(&self.conn, &Channel::Audio.service(&self.id), codec, source, self.config.max_message[2], timeout).await
    }

    /// Holds the device's screen at `rotation`, or gives it back with
    /// `protocol::ROTATION_RELEASE`.
    pub async fn set_rotation(&self, rotation: u8, timeout: Duration) -> Result<()> {
        self.request(SET_ROTATION, &[rotation, 0, 0, 0], timeout).await.map(drop)
    }

    /// Gives the device a resolution of `width`:`height`, or its own back
    /// with 0×0. `Error::Status(ESTATE)` when the user set one already.
    pub async fn adapt_display(&self, width: u16, height: u16, timeout: Duration) -> Result<()> {
        self.request(ADAPT_DISPLAY, &protocol::adapt_display(width, height), timeout).await.map(drop)
    }

    /// The name of each of `packages` drawn `text_px` large, and its icon
    /// `icon_px` square, none with 0.
    pub async fn app_info(&self, icon_px: u16, text_px: u8, packages: &[&str], timeout: Duration) -> Result<AppInfoReply> {
        let args = protocol::get_app_info(icon_px, text_px, packages).ok_or(Error::TooLarge)?;
        let payload = self.request(GET_APP_INFO, &args, timeout).await?;
        protocol::app_entries(&payload, packages.len()).ok_or(Error::Protocol)?;
        Ok(AppInfoReply { payload, count: packages.len() })
    }

    /// Events of type `kind`, starting with the newest that came before.
    pub fn watch(&self, kind: u8) -> EventWatch {
        EventWatch { shared: self.shared.clone(), kind, seen: 0 }
    }

    pub async fn media_state(&self, timeout: Duration) -> Result<MediaState> {
        let reply = self.request(GET_MEDIA, &[], timeout).await?;
        MediaState::parse(&reply).ok_or(Error::Protocol)
    }

    /// The playing track's title and `subtitle` (`protocol::SUBTITLE_*`) drawn
    /// that large, and its art `art_px` square, none with 0.
    pub async fn media_render(&self, title_px: u8, subtitle_px: u8, art_px: u16, subtitle: u8, timeout: Duration) -> Result<MediaRenderReply> {
        let payload = self.request(GET_MEDIA_RENDER, &protocol::get_media_render(title_px, subtitle_px, art_px, subtitle), timeout).await?;
        MediaRender::parse(&payload).ok_or(Error::Protocol)?;
        Ok(MediaRenderReply { payload })
    }

    /// `Error::Status(ESTATE)` without a session, or for a seek it does not take.
    pub async fn media_control(&self, action: MediaAction, timeout: Duration) -> Result<()> {
        self.request(MEDIA_CONTROL, &protocol::media_control(action), timeout).await.map(drop)
    }

    async fn rendered(&self, kind: u8, args: &[u8], timeout: Duration) -> Result<RenderedReply> {
        let payload = self.request(kind, args, timeout).await?;
        Rendered::parse(&payload).ok_or(Error::Protocol)?;
        Ok(RenderedReply { payload })
    }

    /// Takes the device's screen, which the agent keeps for the requests
    /// below, and answers it fitted into the box.
    pub async fn screenshot(&self, box_width: u16, box_height: u16, quality: u8, timeout: Duration) -> Result<RenderedReply> {
        self.rendered(SCREENSHOT, &protocol::screenshot(box_width, box_height, quality), timeout).await
    }

    /// The screenshot kept, fitted into another box; `Error::Status(ESTATE)`
    /// without one.
    pub async fn screenshot_render(&self, box_width: u16, box_height: u16, quality: u8, timeout: Duration) -> Result<RenderedReply> {
        self.rendered(SCREENSHOT_RENDER, &protocol::screenshot(box_width, box_height, quality), timeout).await
    }

    /// The size and the device path of a PNG of the screenshot kept, which
    /// lasts as long as the screenshot.
    pub async fn screenshot_save(&self, timeout: Duration) -> Result<(u64, alloc::string::String)> {
        let payload = self.request(SCREENSHOT_SAVE, &[], timeout).await?;
        protocol::saved(&payload).ok_or(Error::Protocol)
    }

    pub async fn screenshot_release(&self, timeout: Duration) -> Result<()> {
        self.request(SCREENSHOT_RELEASE, &[], timeout).await.map(drop)
    }

    /// The device file at `path` fitted into the box, and its attributes;
    /// `flags` takes `protocol::RENDER_*`. `Error::Status(ENOTSUP)` for a
    /// kind of file the agent does not render.
    pub async fn render_file(&self, path: &str, box_width: u16, box_height: u16, quality: u8, flags: u8, timeout: Duration) -> Result<RenderedReply> {
        self.rendered(RENDER_FILE, &protocol::render_file(box_width, box_height, quality, flags, path), timeout).await
    }

    pub fn is_closed(&self) -> bool {
        self.shared.closed.load(Ordering::Acquire)
    }

    /// Completes with why the link closed.
    pub async fn closed(&self) -> Error {
        poll_fn(|cx| {
            let mut status = self.shared.status.lock();
            if let Some(err) = &status.error {
                return Poll::Ready(err.clone());
            }
            if !status.waiters.iter().any(|w| w.will_wake(cx.waker())) {
                status.waiters.push(cx.waker().clone());
            }
            Poll::Pending
        })
        .await
    }

    pub fn close(&self) {
        self.shared.shut(Error::Closed);
        self.tasks.abort();
    }
}
