/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::vec::Vec;
use core::alloc::Allocator;
use core::time::Duration;

use adb::link::{Connection, Stream};
use idf::executor;

use crate::protocol::{self, CODEC_JPEG, FRAME, FRAME_HEAD, FrameHead, HEADER_BYTES, Header, OK, REPLY, VIDEO_START, VideoStart};
use crate::{Error, Result};

/// The video stream of an agent; dropping it closes the stream.
pub struct Video {
    stream: Stream,
    max: u32,
}

impl Video {
    pub(crate) async fn open(conn: &Connection, service: &str, start: &VideoStart, max: u32, timeout: Duration) -> Result<Self> {
        let mut stream = conn.open(service).await?;
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

    pub async fn dbg_discard_frame(&mut self) -> Result<usize> {
        let header = self.read_header().await?;
        self.skip(header.length as usize).await?;
        Ok(header.length as usize)
    }

    /// The next frame, its JPEG left in `jpeg`.
    pub async fn next_frame<A: Allocator>(&mut self, jpeg: &mut Vec<u8, A>) -> Result<FrameHead> {
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
            jpeg.clear();
            jpeg.try_reserve(length - FRAME_HEAD).map_err(|_| Error::NoMemory)?;
            jpeg.resize(length - FRAME_HEAD, 0);
            self.stream.read_exact(jpeg).await?;
            return Ok(FrameHead::parse(&head));
        }
    }
}
