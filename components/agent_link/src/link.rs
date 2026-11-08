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

use crate::protocol::{self, Channel, HEADER_BYTES, ADAPT_DISPLAY, HELLO, Header, Hello, HelloReply, INPUT_CLEAR, KEY, OK, PROTO_VERSION, REPLY, SET_ROTATION, TOUCH, Touch, VideoStart};
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

struct Shared {
    closed: AtomicBool,
    max_send: AtomicU32,
    outbox: Mutex<Outbox>,
    requests: Mutex<Requests>,
    status: Mutex<Status>,
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
