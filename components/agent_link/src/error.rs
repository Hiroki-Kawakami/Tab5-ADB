/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use core::fmt;

#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Error {
    Adb(adb::Error),
    /// The agent broke the protocol.
    Protocol,
    /// The agent answered with this status, not `OK`.
    Status(u8),
    Timeout,
    /// Larger than the agent takes.
    TooLarge,
    /// `Link::close`, or the agent closed the link.
    Closed,
    NoMemory,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Adb(e) => write!(f, "adb: {e}"),
            Error::Protocol => f.write_str("protocol error"),
            Error::Status(s) => write!(f, "status {s:#04x}"),
            Error::Timeout => f.write_str("timed out"),
            Error::TooLarge => f.write_str("message too large"),
            Error::Closed => f.write_str("link closed"),
            Error::NoMemory => f.write_str("out of memory"),
        }
    }
}

impl From<adb::Error> for Error {
    fn from(e: adb::Error) -> Self {
        match e {
            adb::Error::Closed => Error::Closed,
            e => Error::Adb(e),
        }
    }
}

pub type Result<T> = core::result::Result<T, Error>;
