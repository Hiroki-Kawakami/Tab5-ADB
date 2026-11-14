/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::vec::Vec;
use core::time::Duration;

use adb::link::{Connection, Stream};
use idf::executor;

use crate::protocol::{self, AUDIO, AUDIO_START, AudioCodec, AudioFormat, AudioSource, AudioVolume, HEADER_BYTES, Header, OK, REPLY, VOLUME};
use crate::link::Activity;
use crate::{Error, Result};

/// What the device may queue ahead of the reader; more only arrives late.
const WINDOW_MS: u32 = 200;

/// The stream's bytes per second at the rates the protocol fixes, framing included.
fn bytes_per_second(codec: AudioCodec) -> u32 {
    let header = HEADER_BYTES as u32;
    match codec {
        AudioCodec::Pcm => 48000 * 2 * 2 + 100 * header,
        AudioCodec::Opus => 96000 / 8 + 50 * header,
        AudioCodec::Aac => 128000 / 8 + 47 * header,
    }
}

pub enum AudioMessage {
    /// PCM or one Opus packet, as the format says.
    Data(Vec<u8>),
    /// The device's media volume step, of `max`; while muted, the step it unmutes to.
    Volume { volume: u8, max: u8, muted: bool },
}

/// The audio stream of an agent; dropping it closes the stream, and the device
/// plays its sound again.
pub struct Audio {
    stream: Stream,
    max: u32,
    format: AudioFormat,
}

impl Audio {
    pub(crate) async fn open(conn: &Connection, service: &str, codec: AudioCodec, source: AudioSource, volume: AudioVolume, max: u32, activity: &Activity, timeout: Duration) -> Result<Self> {
        let mut stream = conn.open_with_window(service, bytes_per_second(codec) * WINDOW_MS / 1000).await?;
        let mut out = Vec::new();
        protocol::message(&mut out, AUDIO_START, 0, &protocol::audio_start(codec, source, volume));
        stream.write(&out).await?;
        activity.wrote();
        let reply = match executor::timeout(timeout, read_reply(&mut stream, max)).await {
            Ok(reply) => reply?,
            Err(_) => return Err(Error::Timeout),
        };
        let (status, result) = protocol::reply(&reply).ok_or(Error::Protocol)?;
        if status != OK {
            return Err(Error::Status(status));
        }
        let format = AudioFormat::parse(result).ok_or(Error::Protocol)?;
        Ok(Self { stream, max, format })
    }

    pub fn format(&self) -> AudioFormat {
        self.format
    }

    pub async fn next(&mut self) -> Result<AudioMessage> {
        loop {
            let header = read_header(&mut self.stream, self.max).await?;
            let mut payload = alloc::vec![0u8; header.length as usize];
            self.stream.read_exact(&mut payload).await?;
            match header.kind {
                AUDIO => return Ok(AudioMessage::Data(payload)),
                VOLUME if payload.len() >= 3 => return Ok(AudioMessage::Volume { volume: payload[0], max: payload[1], muted: payload[2] != 0 }),
                _ => {}
            }
        }
    }
}

async fn read_header(stream: &mut Stream, max: u32) -> Result<Header> {
    let mut head = [0u8; HEADER_BYTES];
    stream.read_exact(&mut head).await?;
    let header = Header::parse(&head);
    if header.length > max {
        return Err(Error::Protocol);
    }
    Ok(header)
}

async fn read_reply(stream: &mut Stream, max: u32) -> Result<Vec<u8>> {
    loop {
        let header = read_header(stream, max).await?;
        let mut payload = alloc::vec![0u8; header.length as usize];
        stream.read_exact(&mut payload).await?;
        if header.kind == REPLY {
            return Ok(payload);
        }
    }
}
