/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::vec::Vec;
use core::time::Duration;

use adb::link::{Connection, Slice, Stream};
use idf::executor;

use crate::protocol::{self, CODEC_JPEG, FRAME, FRAME_HEAD, FrameHead, HEADER_BYTES, Header, OK, REPLY, VIDEO_START, VideoStart};
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

/// The video stream of an agent; dropping it closes the stream.
pub struct Video {
    stream: Stream,
    max: u32,
}

impl Video {
    pub(crate) async fn open(conn: &Connection, service: &str, start: &VideoStart, max: u32, timeout: Duration) -> Result<Self> {
        let mut stream = conn.open_with_window(service, WINDOW).await?;
        let mut out = Vec::new();
        protocol::message(&mut out, VIDEO_START, 0, &start.encode());
        stream.write(&out).await?;
        let mut video = Self { stream, max };
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
        Ok(video)
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
            return Ok(Frame { head, jpeg: Jpeg(parts) });
        }
    }
}
