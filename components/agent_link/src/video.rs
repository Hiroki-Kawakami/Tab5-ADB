/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::sync::Arc;
use alloc::vec::Vec;
use core::future::poll_fn;
use core::task::{Poll, Waker};
use core::time::Duration;

use adb::link::{Connection, Slice, StreamReader, StreamWriter};
use idf::executor::{self, JoinHandle, Spawner};
use idf::sync::Mutex;

use crate::protocol::{self, CODEC_JPEG, FRAME, FRAME_ACK, FRAME_HEAD, FrameHead, HEADER_BYTES, Header, OK, REPLY, VIDEO_START, VideoStart};
use crate::{Error, Result};

const WINDOW: u32 = 512 * 1024;

/// A frame's JPEG, lent in the pieces the transport received it in.
pub struct Jpeg(Vec<Slice>);

impl Jpeg {
    pub fn parts(&self) -> Vec<&[u8]> {
        self.0.iter().map(|s| &s[..]).collect()
    }

    pub fn len(&self) -> usize {
        self.0.iter().map(|s| s.len()).sum()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

pub struct Frame {
    pub head: FrameHead,
    pub jpeg: Jpeg,
}

#[derive(Default)]
struct Acks {
    /// Bytes of FRAME messages read, wrapping.
    received: u32,
    sent: u32,
    waker: Option<Waker>,
}

/// FRAME_ACK for what was read, apart from reading: without delayed ack a
/// write waits for the device, and acks coalesce while one is out.
async fn ack_loop(acks: Arc<Mutex<Acks>>, mut writer: StreamWriter) {
    let mut out = Vec::new();
    loop {
        let received = poll_fn(|cx| {
            let mut a = acks.lock();
            if a.received == a.sent {
                a.waker = Some(cx.waker().clone());
                return Poll::Pending;
            }
            a.sent = a.received;
            Poll::Ready(a.received)
        })
        .await;
        out.clear();
        protocol::message(&mut out, FRAME_ACK, 0, &received.to_le_bytes());
        if writer.write(&out).await.is_err() {
            return;
        }
    }
}

/// The video stream of an agent; dropping it closes the stream.
pub struct Video {
    stream: StreamReader,
    max: u32,
    acks: Arc<Mutex<Acks>>,
    task: Option<JoinHandle<()>>,
}

impl Video {
    pub(crate) async fn open(conn: &Connection, spawner: &Spawner, service: &str, start: &VideoStart, max: u32, timeout: Duration) -> Result<Self> {
        let (stream, mut writer) = conn.open_with_window(service, WINDOW).await?.into_split();
        let mut out = Vec::new();
        protocol::message(&mut out, VIDEO_START, 0, &start.encode());
        writer.write(&out).await?;
        let mut video = Self { stream, max, acks: Arc::new(Mutex::new(Acks::default())), task: None };
        let reply = match executor::timeout(timeout, video.read_reply()).await {
            Ok(reply) => reply?,
            Err(_) => return Err(Error::Timeout),
        };
        let (status, result) = protocol::reply(&reply).ok_or(Error::Protocol)?;
        if status != OK {
            return Err(Error::Status(status));
        }
        if result.first() != Some(&CODEC_JPEG) {
            return Err(Error::Protocol);
        }
        video.task = Some(spawner.spawn(ack_loop(video.acks.clone(), writer)));
        Ok(video)
    }

    fn received(&self, bytes: usize) {
        let mut a = self.acks.lock();
        a.received = a.received.wrapping_add(bytes as u32);
        if let Some(waker) = a.waker.take() {
            waker.wake();
        }
    }

    async fn read_header(&mut self) -> Result<Header> {
        let mut head = [0u8; HEADER_BYTES];
        self.stream.read_exact(&mut head).await?;
        let header = Header::parse(&head);
        if header.length > self.max {
            return Err(Error::Protocol);
        }
        Ok(header)
    }

    async fn skip(&mut self, mut len: usize) -> Result<()> {
        let mut chunk = [0u8; 256];
        while len > 0 {
            let n = len.min(chunk.len());
            self.stream.read_exact(&mut chunk[..n]).await?;
            len -= n;
        }
        Ok(())
    }

    async fn read_reply(&mut self) -> Result<Vec<u8>> {
        loop {
            let header = self.read_header().await?;
            if header.kind != REPLY {
                self.skip(header.length as usize).await?;
                continue;
            }
            let mut payload = alloc::vec![0u8; header.length as usize];
            self.stream.read_exact(&mut payload).await?;
            return Ok(payload);
        }
    }

    pub async fn next_frame(&mut self) -> Result<Frame> {
        loop {
            let header = self.read_header().await?;
            let length = header.length as usize;
            if header.kind != FRAME {
                self.skip(length).await?;
                continue;
            }
            if length < FRAME_HEAD {
                return Err(Error::Protocol);
            }
            let mut head = [0u8; FRAME_HEAD];
            self.stream.read_exact(&mut head).await?;
            let head = FrameHead::parse(&head);
            let mut left = length - FRAME_HEAD;
            let mut parts = Vec::new();
            while left > 0 {
                let part = self.stream.read_slice(left).await?;
                if part.is_empty() {
                    return Err(Error::Closed);
                }
                left -= part.len();
                parts.try_reserve(1).map_err(|_| Error::NoMemory)?;
                parts.push(part);
            }
            self.received(HEADER_BYTES + length);
            return Ok(Frame { head, jpeg: Jpeg(parts) });
        }
    }
}

impl Drop for Video {
    fn drop(&mut self) {
        if let Some(task) = self.task.take() {
            task.abort();
        }
    }
}
