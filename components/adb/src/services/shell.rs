/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::format;
use alloc::vec::Vec;

use crate::link::{Connection, Stream, StreamReader, StreamWriter};
use crate::{Error, Result};

const STDIN: u8 = 0;
const STDOUT: u8 = 1;
const STDERR: u8 = 2;
const EXIT: u8 = 3;
const WINDOW_SIZE: u8 = 5;
const PACKET_MAX: usize = 1024 * 1024;

/// Runs `command` and returns what it printed, stdout and stderr together,
/// once it exits.
pub async fn exec(conn: &Connection, command: &str) -> Result<Vec<u8>> {
    shell(conn, command).await?.read_to_end().await
}

/// A shell running `command`, or an interactive one on a terminal when it is
/// empty. Its output arrives as it is, stdout and stderr together.
pub async fn shell(conn: &Connection, command: &str) -> Result<Stream> {
    conn.open(&format!("shell:{command}")).await
}

/// `command` on a terminal of type `term`, or an interactive shell when it is
/// empty, over the shell protocol (`shell_v2`).
pub async fn pty(conn: &Connection, term: &str, command: &str) -> Result<(PtyReader, PtyWriter)> {
    let (reader, writer) = conn.open(&format!("shell,v2,TERM={term},pty:{command}")).await?.into_split();
    Ok((PtyReader { reader }, PtyWriter { writer }))
}

pub enum PtyOutput {
    Data(Vec<u8>),
    /// The last thing a session sends.
    Exit(u8),
}

pub struct PtyReader {
    reader: StreamReader,
}

impl PtyReader {
    /// `Closed` when the device ends the session without an exit code.
    pub async fn read(&mut self) -> Result<PtyOutput> {
        loop {
            let mut head = [0u8; 5];
            self.reader.read_exact(&mut head).await?;
            let len = u32::from_le_bytes([head[1], head[2], head[3], head[4]]) as usize;
            if len > PACKET_MAX {
                return Err(Error::Protocol);
            }
            let mut data = alloc::vec![0u8; len];
            self.reader.read_exact(&mut data).await?;
            match head[0] {
                STDOUT | STDERR => return Ok(PtyOutput::Data(data)),
                EXIT => return Ok(PtyOutput::Exit(data.first().copied().unwrap_or(0))),
                _ => {}
            }
        }
    }
}

pub struct PtyWriter {
    writer: StreamWriter,
}

impl PtyWriter {
    pub async fn write(&mut self, data: &[u8]) -> Result<()> {
        self.writer.write(&packet(STDIN, data)).await
    }

    pub async fn resize(&mut self, rows: u16, cols: u16) -> Result<()> {
        self.writer.write(&packet(WINDOW_SIZE, &window_size(rows, cols))).await
    }

    pub async fn close(self) -> Result<()> {
        self.writer.close().await
    }
}

pub(crate) fn packet(id: u8, data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(5 + data.len());
    out.push(id);
    out.extend_from_slice(&(data.len() as u32).to_le_bytes());
    out.extend_from_slice(data);
    out
}

/// `struct winsize` as the daemon scans it, NUL included.
pub(crate) fn window_size(rows: u16, cols: u16) -> Vec<u8> {
    format!("{rows}x{cols},0x0\0").into_bytes()
}
