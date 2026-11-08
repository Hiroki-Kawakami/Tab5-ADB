/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * The sync: service, version 1 (32-bit sizes).
 */

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

use crate::link::{Connection, Stream};
use crate::{Error, Result};

const STAT: &[u8; 4] = b"STAT";
const STA2: &[u8; 4] = b"STA2";
const LIST: &[u8; 4] = b"LIST";
const DENT: &[u8; 4] = b"DENT";
const SEND: &[u8; 4] = b"SEND";
const RECV: &[u8; 4] = b"RECV";
const DATA: &[u8; 4] = b"DATA";
const DONE: &[u8; 4] = b"DONE";
const OKAY: &[u8; 4] = b"OKAY";
const FAIL: &[u8; 4] = b"FAIL";
const QUIT: &[u8; 4] = b"QUIT";

/// The most one DATA message carries.
pub const DATA_MAX: usize = 64 * 1024;
/// Device feature that `stat_v2` needs.
pub const FEATURE_STAT_V2: &str = "stat_v2";
const FAIL_MAX: usize = 4096;

const S_IFMT: u32 = 0o170000;
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const S_IFLNK: u32 = 0o120000;

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct FileStat {
    /// `st_mode`; 0 when the path does not exist.
    pub mode: u32,
    pub size: u64,
    /// Seconds since 1970.
    pub mtime: u32,
}

impl FileStat {
    pub fn exists(&self) -> bool {
        self.mode != 0
    }

    pub fn is_dir(&self) -> bool {
        self.mode & S_IFMT == S_IFDIR
    }

    pub fn is_file(&self) -> bool {
        self.mode & S_IFMT == S_IFREG
    }

    pub fn is_symlink(&self) -> bool {
        self.mode & S_IFMT == S_IFLNK
    }
}

/// A symlink is listed as one, not followed.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct DirEntry {
    pub name: String,
    pub stat: FileStat,
}

fn word(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

const STAT_V2_SIZE: usize = 72;

pub(crate) fn stat_v2(reply: &[u8; STAT_V2_SIZE]) -> Result<FileStat> {
    if &reply[..4] != STA2 {
        return Err(Error::Protocol);
    }
    if word(reply, 4) != 0 {
        return Ok(FileStat::default());
    }
    let size = u64::from(word(reply, 40)) | u64::from(word(reply, 44)) << 32;
    Ok(FileStat { mode: word(reply, 24), size, mtime: word(reply, 56) })
}

fn request(id: &[u8; 4], body: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(8 + body.len());
    out.extend_from_slice(id);
    out.extend_from_slice(&(body.len() as u32).to_le_bytes());
    out.extend_from_slice(body);
    out
}

/// One session; requests run one at a time. A failed transfer may leave the
/// session unusable: open another.
pub struct Sync {
    stream: Stream,
}

impl Sync {
    pub async fn open(conn: &Connection) -> Result<Self> {
        Ok(Self { stream: conn.open("sync:").await? })
    }

    pub async fn stat(&mut self, path: &str) -> Result<FileStat> {
        self.stream.write(&request(STAT, path.as_bytes())).await?;
        let mut reply = [0u8; 16];
        self.stream.read_exact(&mut reply).await?;
        if &reply[..4] != STAT {
            return Err(Error::Protocol);
        }
        Ok(FileStat { mode: word(&reply, 4), size: word(&reply, 8).into(), mtime: word(&reply, 12) })
    }

    /// Follows symlinks, unlike `stat`; a dangling one does not exist. Only
    /// on devices with `FEATURE_STAT_V2`.
    pub async fn stat_v2(&mut self, path: &str) -> Result<FileStat> {
        self.stream.write(&request(STA2, path.as_bytes())).await?;
        let mut reply = [0u8; STAT_V2_SIZE];
        self.stream.read_exact(&mut reply).await?;
        stat_v2(&reply)
    }

    /// Includes `.` and `..` as the device sends them.
    pub async fn list(&mut self, path: &str) -> Result<Vec<DirEntry>> {
        self.stream.write(&request(LIST, path.as_bytes())).await?;
        let mut entries = Vec::new();
        loop {
            let mut head = [0u8; 20];
            self.stream.read_exact(&mut head).await?;
            match &head[..4] {
                id if id == DONE => return Ok(entries),
                id if id == DENT => {
                    let mut name = alloc::vec![0u8; word(&head, 16) as usize];
                    self.stream.read_exact(&mut name).await?;
                    let stat = FileStat { mode: word(&head, 4), size: word(&head, 8).into(), mtime: word(&head, 12) };
                    entries.push(DirEntry { name: String::from_utf8_lossy(&name).into_owned(), stat });
                }
                _ => return Err(Error::Protocol),
            }
        }
    }

    /// Writes `path` with permission bits `mode` (such as 0o644) and `mtime`
    /// from what `source` fills in, until it returns 0.
    pub async fn push(
        &mut self,
        path: &str,
        mode: u32,
        mtime: u32,
        mut source: impl FnMut(&mut [u8]) -> Result<usize>,
    ) -> Result<()> {
        self.stream.write(&request(SEND, format!("{path},{mode}").as_bytes())).await?;
        let mut message = alloc::vec![0u8; 8 + DATA_MAX];
        message[..4].copy_from_slice(DATA);
        loop {
            let n = source(&mut message[8..])?.min(DATA_MAX);
            if n == 0 {
                break;
            }
            message[4..8].copy_from_slice(&(n as u32).to_le_bytes());
            self.stream.write(&message[..8 + n]).await?;
        }
        let mut done = [0u8; 8];
        done[..4].copy_from_slice(DONE);
        done[4..].copy_from_slice(&mtime.to_le_bytes());
        self.stream.write(&done).await?;
        self.status().await
    }

    pub async fn push_bytes(&mut self, path: &str, mode: u32, mtime: u32, data: &[u8]) -> Result<()> {
        let mut rest = data;
        self.push(path, mode, mtime, |buf| {
            let n = buf.len().min(rest.len());
            buf[..n].copy_from_slice(&rest[..n]);
            rest = &rest[n..];
            Ok(n)
        })
        .await
    }

    /// Hands `path`'s content to `sink` as it arrives. An error from `sink`
    /// leaves the session unusable, since the device keeps sending.
    pub async fn pull(&mut self, path: &str, mut sink: impl FnMut(&[u8]) -> Result<()>) -> Result<()> {
        self.stream.write(&request(RECV, path.as_bytes())).await?;
        let mut data = Vec::new();
        loop {
            let mut head = [0u8; 8];
            self.stream.read_exact(&mut head).await?;
            let len = word(&head, 4) as usize;
            match &head[..4] {
                id if id == DONE => return Ok(()),
                id if id == DATA && len <= DATA_MAX => {
                    data.resize(len, 0);
                    self.stream.read_exact(&mut data).await?;
                    sink(&data)?;
                }
                id if id == FAIL => return Err(self.failure(len).await),
                _ => return Err(Error::Protocol),
            }
        }
    }

    pub async fn close(mut self) -> Result<()> {
        self.stream.write(&request(QUIT, &[])).await?;
        self.stream.close().await
    }

    async fn status(&mut self) -> Result<()> {
        let mut head = [0u8; 8];
        self.stream.read_exact(&mut head).await?;
        match &head[..4] {
            id if id == OKAY => Ok(()),
            id if id == FAIL => Err(self.failure(word(&head, 4) as usize).await),
            _ => Err(Error::Protocol),
        }
    }

    async fn failure(&mut self, len: usize) -> Error {
        if len > FAIL_MAX {
            return Error::Protocol;
        }
        let mut message = alloc::vec![0u8; len];
        match self.stream.read_exact(&mut message).await {
            Ok(()) => Error::Sync(String::from_utf8_lossy(&message).into_owned()),
            Err(err) => err,
        }
    }
}
