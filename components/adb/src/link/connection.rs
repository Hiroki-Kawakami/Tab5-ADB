/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::boxed::Box;
use alloc::collections::{BTreeMap, VecDeque};
use alloc::string::String;
use alloc::sync::{Arc, Weak};
use alloc::vec::Vec;
use core::future::poll_fn;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use core::task::{Poll, Waker};

use idf::executor::{JoinHandle, Spawner};
use idf::sync::Mutex;

use crate::link::identity::Identity;
use crate::link::packet::{
    A_AUTH, A_CLSE, A_CNXN, A_OKAY, A_OPEN, A_STLS, A_STLS_VERSION, A_VERSION, A_WRTE, AUTH_RSAPUBLICKEY, AUTH_SIGNATURE, AUTH_TOKEN, Chunk, Packet, Payload, Slice,
};
use crate::link::transport::{PacketReader, PacketWriter, Transport};
use crate::{Error, Result};

const MAX_PAYLOAD: u32 = 256 * 1024;
const BANNER: &[u8] = b"host::features=shell_v2,cmd\0";

#[derive(Clone, PartialEq, Eq, Debug)]
pub enum State {
    /// CNXN sent.
    Connecting,
    /// The device asked for a signature with our key.
    Authorizing,
    /// The device did not know the key and has it now; it asks its user. A
    /// refusal sends nothing, so this lasts until `close`.
    Unauthorized,
    Online,
    Closed(Error),
}

fn wake_all(wakers: &mut Vec<Waker>) {
    for waker in core::mem::take(wakers) {
        waker.wake();
    }
}

fn register(wakers: &mut Vec<Waker>, waker: &Waker) {
    if !wakers.iter().any(|w| w.will_wake(waker)) {
        wakers.push(waker.clone());
    }
}

struct Status {
    state: State,
    version: u32,
    waiters: Vec<Waker>,
}

struct Shared {
    spawner: Spawner,
    status: Mutex<Status>,
    writer: Mutex<Option<Box<dyn PacketWriter>>>,
    closed: AtomicBool,
    streams: Mutex<BTreeMap<u32, Arc<StreamShared>>>,
    next_local: AtomicU32,
    max_payload: AtomicU32,
    banner: Mutex<String>,
    reader: Mutex<Option<JoinHandle<()>>>,
}

impl Shared {
    fn state(&self) -> (State, u32) {
        let status = self.status.lock();
        (status.state.clone(), status.version)
    }

    fn set_state(&self, state: State) {
        let mut status = self.status.lock();
        if matches!(status.state, State::Closed(_)) || status.state == state {
            return;
        }
        status.state = state;
        status.version = status.version.wrapping_add(1);
        wake_all(&mut status.waiters);
    }

    /// Ends the connection with `error` and wakes everything waiting on it.
    fn shut(&self, error: Error) {
        if self.closed.swap(true, Ordering::AcqRel) {
            return;
        }
        self.set_state(State::Closed(error));
        let streams = core::mem::take(&mut *self.streams.lock());
        for stream in streams.into_values() {
            stream.finish();
        }
    }

    fn close(&self, error: Error) {
        self.shut(error);
        if let Some(reader) = self.reader.lock().take() {
            reader.abort();
        }
    }

    async fn send(&self, packet: Packet) -> Result<()> {
        if self.closed.load(Ordering::Acquire) {
            return Err(Error::NotOnline);
        }
        let mut writer = self.writer.lock_async().await;
        let Some(w) = writer.as_mut() else { return Err(Error::NotOnline) };
        if let Err(err) = w.write(&packet).await {
            drop(writer);
            self.shut(err.clone());
            return Err(err);
        }
        Ok(())
    }

    async fn handle(&self, p: Packet, identity: &Identity, signed: &mut bool, offered: &mut bool) -> Result<()> {
        match p.command {
            A_CNXN => {
                self.max_payload.store(p.arg1.min(MAX_PAYLOAD), Ordering::Release);
                let payload = p.payload.into_vec();
                let end = payload.iter().position(|&b| b == 0).unwrap_or(payload.len());
                *self.banner.lock() = String::from_utf8_lossy(&payload[..end]).into_owned();
                self.set_state(State::Online);
            }
            A_AUTH if p.arg0 == AUTH_TOKEN => {
                if !*signed {
                    *signed = true;
                    let signature = identity.sign_token(&p.payload.into_vec())?;
                    self.set_state(State::Authorizing);
                    self.send(Packet::new(A_AUTH, AUTH_SIGNATURE, 0, signature)).await?;
                } else if !*offered {
                    *offered = true;
                    let key = identity.public_key_payload();
                    self.send(Packet::new(A_AUTH, AUTH_RSAPUBLICKEY, 0, key)).await?;
                    self.set_state(State::Unauthorized);
                }
            }
            A_OKAY => {
                let stream = self.streams.lock().get(&p.arg1).cloned();
                if let Some(stream) = stream {
                    stream.okay(p.arg0);
                }
            }
            A_WRTE => {
                let stream = self.streams.lock().get(&p.arg1).cloned();
                match stream {
                    Some(stream) => stream.deliver(p.payload),
                    None => self.send(Packet::new(A_CLSE, 0, p.arg0, Vec::new())).await?,
                }
            }
            A_CLSE => {
                let stream = self.streams.lock().remove(&p.arg1);
                if let Some(stream) = stream {
                    stream.finish();
                }
            }
            _ => {}
        }
        Ok(())
    }
}

async fn read_loop(shared: Weak<Shared>, mut reader: Box<dyn PacketReader>, identity: Arc<Identity>) {
    {
        let Some(s) = shared.upgrade() else { return };
        if s.send(Packet::new(A_CNXN, A_VERSION, MAX_PAYLOAD, BANNER.into())).await.is_err() {
            return;
        }
    }
    let (mut signed, mut offered, mut tls) = (false, false, false);
    loop {
        let packet = reader.read().await;
        let Some(s) = shared.upgrade() else { return };
        let result = match packet {
            Ok(p) if p.command == A_STLS => {
                if tls {
                    Err(Error::Protocol)
                } else {
                    tls = true;
                    match s.send(Packet::new(A_STLS, A_STLS_VERSION, 0, Vec::new())).await {
                        Ok(()) => reader.start_tls(&identity).await,
                        Err(err) => Err(err),
                    }
                }
            }
            Ok(p) => s.handle(p, &identity, &mut signed, &mut offered).await,
            Err(err) => Err(err),
        };
        if let Err(err) = result {
            s.shut(err);
            return;
        }
    }
}

/// Closes the connection once the last `Connection` and `Stream` are gone.
struct Handle(Arc<Shared>);

impl Drop for Handle {
    fn drop(&mut self) {
        self.0.close(Error::Canceled);
    }
}

/// One device's ADB link. Clones share it; the last one dropped, with every
/// stream it opened, closes it.
pub struct Connection {
    handle: Arc<Handle>,
    seen: AtomicU32,
}

impl Clone for Connection {
    fn clone(&self) -> Self {
        Self { handle: self.handle.clone(), seen: AtomicU32::new(self.seen.load(Ordering::Relaxed)) }
    }
}

impl Connection {
    /// Returns at once; the handshake runs on `spawner`. Follow it with
    /// `state` and `state_changed`, or `online`.
    pub fn connect(transport: Box<dyn Transport>, identity: Arc<Identity>, spawner: &Spawner) -> Self {
        let (reader, writer) = transport.split();
        let shared = Arc::new(Shared {
            spawner: spawner.clone(),
            status: Mutex::new(Status { state: State::Connecting, version: 0, waiters: Vec::new() }),
            writer: Mutex::new(Some(writer)),
            closed: AtomicBool::new(false),
            streams: Mutex::new(BTreeMap::new()),
            next_local: AtomicU32::new(1),
            max_payload: AtomicU32::new(MAX_PAYLOAD),
            banner: Mutex::new(String::new()),
            reader: Mutex::new(None),
        });
        let task = spawner.spawn(read_loop(Arc::downgrade(&shared), reader, identity));
        *shared.reader.lock() = Some(task);
        Self { handle: Arc::new(Handle(shared)), seen: AtomicU32::new(0) }
    }

    fn shared(&self) -> &Arc<Shared> {
        &self.handle.0
    }

    pub fn state(&self) -> State {
        let (state, version) = self.shared().state();
        self.seen.store(version, Ordering::Relaxed);
        state
    }

    /// Completes once the state differs from what `state` last returned
    /// through this `Connection`.
    pub async fn state_changed(&self) {
        let seen = self.seen.load(Ordering::Relaxed);
        poll_fn(|cx| {
            let mut status = self.shared().status.lock();
            if status.version != seen {
                return Poll::Ready(());
            }
            register(&mut status.waiters, cx.waker());
            Poll::Pending
        })
        .await
    }

    /// Waits for `Online`, with no time limit, like `adb wait-for-device`.
    pub async fn online(&self) -> Result<()> {
        loop {
            match self.state() {
                State::Online => return Ok(()),
                State::Closed(err) => return Err(err),
                _ => self.state_changed().await,
            }
        }
    }

    /// What the device sent with its CNXN; empty before `Online`.
    pub fn banner(&self) -> String {
        self.shared().banner.lock().clone()
    }

    /// Whether the device listed `feature` in its banner.
    pub fn has_feature(&self, feature: &str) -> bool {
        banner_has_feature(&self.banner(), feature)
    }

    /// The largest payload one packet carries either way.
    pub fn max_payload(&self) -> usize {
        self.shared().max_payload.load(Ordering::Acquire) as usize
    }

    /// From any state; streams end and waiters return.
    pub fn close(&self) {
        self.shared().close(Error::Canceled);
    }

    /// Opens `service` (`shell:ls`, `sync:`, `localabstract:<name>`, ...).
    pub async fn open(&self, service: &str) -> Result<Stream> {
        if self.shared().state().0 != State::Online {
            return Err(Error::NotOnline);
        }
        let shared = self.shared();
        let local = shared.next_local.fetch_add(1, Ordering::Relaxed);
        let stream = Arc::new(StreamShared::new(local));
        shared.streams.lock().insert(local, stream.clone());
        let guard = Arc::new(Closer { handle: self.handle.clone(), stream: stream.clone() });
        let mut name = Vec::with_capacity(service.len() + 1);
        name.extend_from_slice(service.as_bytes());
        name.push(0);
        shared.send(Packet::new(A_OPEN, local, 0, name)).await?;
        poll_fn(|cx| {
            let mut s = stream.state.lock();
            match s.phase {
                Phase::Opening => {
                    s.opener = Some(cx.waker().clone());
                    Poll::Pending
                }
                _ => Poll::Ready(()),
            }
        })
        .await;
        if !stream.state.lock().accepted {
            return Err(match shared.state().0 {
                State::Closed(err) => err,
                _ => Error::Rejected,
            });
        }
        Ok(Stream {
            reader: StreamReader { guard: guard.clone(), stream: stream.clone() },
            writer: StreamWriter { guard, stream },
        })
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Phase {
    Opening,
    Open,
    Closed,
}

struct StreamState {
    phase: Phase,
    /// The device answered the OPEN with OKAY; the stream may be closed since.
    accepted: bool,
    remote: u32,
    writable: bool,
    inbox: VecDeque<Chunk>,
    /// Into the front of `inbox`.
    read_at: usize,
    /// The device's last WRTE, not acknowledged until it is read.
    unacked: bool,
    opener: Option<Waker>,
    reader: Option<Waker>,
    writer: Option<Waker>,
}

struct StreamShared {
    local: u32,
    state: Mutex<StreamState>,
}

impl StreamShared {
    fn new(local: u32) -> Self {
        Self {
            local,
            state: Mutex::new(StreamState {
                phase: Phase::Opening,
                accepted: false,
                remote: 0,
                writable: false,
                inbox: VecDeque::new(),
                read_at: 0,
                unacked: false,
                opener: None,
                reader: None,
                writer: None,
            }),
        }
    }

    fn wake(s: &mut StreamState) {
        for waker in [s.opener.take(), s.reader.take(), s.writer.take()].into_iter().flatten() {
            waker.wake();
        }
    }

    fn okay(&self, remote: u32) {
        let mut s = self.state.lock();
        match s.phase {
            Phase::Opening => {
                s.phase = Phase::Open;
                s.accepted = true;
                s.remote = remote;
                s.writable = true;
            }
            Phase::Open => s.writable = true,
            Phase::Closed => return,
        }
        Self::wake(&mut s);
    }

    fn deliver(&self, data: Payload) {
        let mut s = self.state.lock();
        s.inbox.extend(data.into_chunks());
        s.unacked = true;
        Self::wake(&mut s);
    }

    fn finish(&self) {
        let mut s = self.state.lock();
        s.phase = Phase::Closed;
        Self::wake(&mut s);
    }
}

/// Sends CLSE once the reader and the writer of a stream are both gone.
struct Closer {
    handle: Arc<Handle>,
    stream: Arc<StreamShared>,
}

impl Closer {
    fn close_now(&self) -> Option<Packet> {
        let shared = &self.handle.0;
        shared.streams.lock().remove(&self.stream.local);
        let mut s = self.stream.state.lock();
        let was = s.phase;
        s.phase = Phase::Closed;
        StreamShared::wake(&mut s);
        (was != Phase::Closed).then(|| Packet::new(A_CLSE, self.stream.local, s.remote, Vec::new()))
    }
}

impl Drop for Closer {
    fn drop(&mut self) {
        if let Some(packet) = self.close_now() {
            let handle = self.handle.clone();
            drop(self.handle.0.spawner.spawn(async move {
                let _ = handle.0.send(packet).await;
            }));
        }
    }
}

/// A service's byte stream both ways.
pub struct Stream {
    reader: StreamReader,
    writer: StreamWriter,
}

impl Stream {
    /// 0 once the device closed it and everything it sent was read.
    pub async fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        self.reader.read(buf).await
    }

    /// `Closed` when the stream ends first.
    pub async fn read_exact(&mut self, buf: &mut [u8]) -> Result<()> {
        self.reader.read_exact(buf).await
    }

    /// Everything until the device closes it.
    pub async fn read_to_end(&mut self) -> Result<Vec<u8>> {
        self.reader.read_to_end().await
    }

    pub async fn read_slice(&mut self, max: usize) -> Result<Slice> {
        self.reader.read_slice(max).await
    }

    /// Returns once the device took all of `data`.
    pub async fn write(&mut self, data: &[u8]) -> Result<()> {
        self.writer.write(data).await
    }

    pub async fn close(self) -> Result<()> {
        self.writer.close().await
    }

    pub fn into_split(self) -> (StreamReader, StreamWriter) {
        (self.reader, self.writer)
    }
}

pub struct StreamReader {
    guard: Arc<Closer>,
    stream: Arc<StreamShared>,
}

impl StreamReader {
    pub async fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        let slice = self.read_slice(buf.len()).await?;
        buf[..slice.len()].copy_from_slice(&slice);
        Ok(slice.len())
    }

    /// Up to `max` bytes of what arrived, without a copy; empty once the
    /// device closed the stream and everything was read.
    pub async fn read_slice(&mut self, max: usize) -> Result<Slice> {
        let (slice, ack) = poll_fn(|cx| {
            let mut s = self.stream.state.lock();
            if let Some(front) = s.inbox.front().cloned() {
                let len = (*front).as_ref().len();
                let at = s.read_at;
                let n = max.min(len - at);
                if at + n == len {
                    s.inbox.pop_front();
                    s.read_at = 0;
                } else {
                    s.read_at += n;
                }
                let ack = (s.inbox.is_empty() && s.unacked && s.phase == Phase::Open).then(|| {
                    s.unacked = false;
                    s.remote
                });
                return Poll::Ready((Slice::new(front, at, at + n), ack));
            }
            if s.phase == Phase::Closed {
                return Poll::Ready((Slice::new(Arc::new(Vec::new()), 0, 0), None));
            }
            s.reader = Some(cx.waker().clone());
            Poll::Pending
        })
        .await;
        if let Some(remote) = ack {
            let packet = Packet::new(A_OKAY, self.stream.local, remote, Vec::new());
            self.guard.handle.0.send(packet).await?;
        }
        Ok(slice)
    }

    pub async fn read_exact(&mut self, mut buf: &mut [u8]) -> Result<()> {
        while !buf.is_empty() {
            let n = self.read(buf).await?;
            if n == 0 {
                return Err(Error::Closed);
            }
            buf = &mut buf[n..];
        }
        Ok(())
    }

    pub async fn read_to_end(&mut self) -> Result<Vec<u8>> {
        let mut out = Vec::new();
        let mut chunk = alloc::vec![0u8; 4096];
        loop {
            let n = self.read(&mut chunk).await?;
            if n == 0 {
                return Ok(out);
            }
            out.extend_from_slice(&chunk[..n]);
        }
    }
}

pub struct StreamWriter {
    guard: Arc<Closer>,
    stream: Arc<StreamShared>,
}

impl StreamWriter {
    pub async fn write(&mut self, data: &[u8]) -> Result<()> {
        let max = self.guard.handle.0.max_payload.load(Ordering::Acquire).max(1) as usize;
        for chunk in data.chunks(max) {
            let remote = poll_fn(|cx| {
                let mut s = self.stream.state.lock();
                match s.phase {
                    Phase::Closed => Poll::Ready(Err(Error::Closed)),
                    Phase::Open if s.writable => {
                        s.writable = false;
                        Poll::Ready(Ok(s.remote))
                    }
                    _ => {
                        s.writer = Some(cx.waker().clone());
                        Poll::Pending
                    }
                }
            })
            .await?;
            let packet = Packet::new(A_WRTE, self.stream.local, remote, chunk.into());
            self.guard.handle.0.send(packet).await?;
        }
        poll_fn(|cx| {
            let mut s = self.stream.state.lock();
            match s.phase {
                Phase::Closed => Poll::Ready(Err(Error::Closed)),
                _ if s.writable => Poll::Ready(Ok(())),
                _ => {
                    s.writer = Some(cx.waker().clone());
                    Poll::Pending
                }
            }
        })
        .await
    }

    /// Sends CLSE now; the reader half sees the end too.
    pub async fn close(self) -> Result<()> {
        match self.guard.close_now() {
            Some(packet) => self.guard.handle.0.send(packet).await,
            None => Ok(()),
        }
    }
}

pub(crate) fn banner_has_feature(banner: &str, feature: &str) -> bool {
    let props = banner.split_once("::").map_or(banner, |(_, props)| props);
    props.split(';').filter_map(|prop| prop.strip_prefix("features=")).flat_map(|list| list.split(',')).any(|f| f == feature)
}
