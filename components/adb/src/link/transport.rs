/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::boxed::Box;
use core::future::Future;
use core::pin::Pin;

use crate::link::identity::Identity;
use crate::{Error, Result};
pub use crate::link::packet::{Chunk, Header, Packet};

pub type BoxFuture<'a, T> = Pin<Box<dyn Future<Output = T> + Send + 'a>>;

/// A link to one device that carries whole packets.
pub trait Transport: Send {
    fn split(self: Box<Self>) -> (Box<dyn PacketReader>, Box<dyn PacketWriter>);
}

/// One reader reads packets in order, each header then all of its payload;
/// an error ends the connection.
pub trait PacketReader: Send {
    fn read_header(&mut self) -> BoxFuture<'_, Result<Header>>;

    /// The next `len` bytes of the payload being read, at most what is left
    /// of it, in a chunk of their own.
    fn read_payload(&mut self, len: usize) -> BoxFuture<'_, Result<Chunk>>;

    /// Turns both halves into TLS presenting `identity`, as A_STLS asks;
    /// the writer is idle meanwhile. Links that cannot fail with `Protocol`.
    fn start_tls<'a>(&'a mut self, _identity: &'a Identity) -> BoxFuture<'a, Result<()>> {
        Box::pin(async { Err(Error::Protocol) })
    }
}

/// Called by one writer at a time.
pub trait PacketWriter: Send {
    fn write<'a>(&'a mut self, packet: &'a Packet) -> BoxFuture<'a, Result<()>>;
}
