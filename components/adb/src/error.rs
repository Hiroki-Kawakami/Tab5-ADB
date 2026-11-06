/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::String;
use core::fmt;

#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Error {
    /// The device went away or its link failed.
    Disconnected,
    /// `Connection::close`, or the last handle dropped.
    Canceled,
    /// Not online yet, or no longer.
    NotOnline,
    /// The device refused to open the service.
    Rejected,
    Closed,
    /// The device broke the protocol.
    Protocol,
    /// The sync service answered FAIL with this message.
    Sync(String),
    Usb(usb_host_rs::Error),
    Crypto(devkit_crypto::Error),
    Storage(idf::Error),
    NoMemory,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Disconnected => f.write_str("disconnected"),
            Error::Canceled => f.write_str("canceled"),
            Error::NotOnline => f.write_str("not online"),
            Error::Rejected => f.write_str("service rejected"),
            Error::Closed => f.write_str("stream closed"),
            Error::Protocol => f.write_str("protocol error"),
            Error::Sync(message) => write!(f, "sync: {message}"),
            Error::Usb(e) => write!(f, "usb: {e}"),
            Error::Crypto(e) => write!(f, "crypto: {e}"),
            Error::Storage(e) => write!(f, "storage: {e}"),
            Error::NoMemory => f.write_str("out of memory"),
        }
    }
}

impl From<devkit_crypto::Error> for Error {
    fn from(e: devkit_crypto::Error) -> Self {
        Error::Crypto(e)
    }
}

pub type Result<T> = core::result::Result<T, Error>;
