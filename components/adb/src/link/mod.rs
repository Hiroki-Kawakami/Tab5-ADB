/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Up to an established connection: transports, authentication, streams.
 */

pub(crate) mod base64;
pub(crate) mod connection;
pub mod identity;
pub(crate) mod packet;
pub mod pair;
pub(crate) mod spake2;
pub mod tcp;
pub mod transport;
pub mod usb;

pub use connection::{Connection, DEFAULT_WINDOW, State, Stream, StreamReader, StreamWriter};
pub use packet::Slice;
pub use identity::Identity;
