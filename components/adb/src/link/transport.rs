/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::boxed::Box;
use core::future::Future;
use core::pin::Pin;

use crate::Result;
pub use crate::link::packet::Packet;

pub type BoxFuture<'a, T> = Pin<Box<dyn Future<Output = T> + Send + 'a>>;

/// A link to one device that carries whole packets.
pub trait Transport: Send {
    fn split(self: Box<Self>) -> (Box<dyn PacketReader>, Box<dyn PacketWriter>);
}

/// One reader reads packets in order; an error ends the connection.
pub trait PacketReader: Send {
    fn read(&mut self) -> BoxFuture<'_, Result<Packet>>;
}

/// Called by one writer at a time.
pub trait PacketWriter: Send {
    fn write<'a>(&'a mut self, packet: &'a Packet) -> BoxFuture<'a, Result<()>>;
}
