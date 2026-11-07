/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::boxed::Box;
use alloc::vec::Vec;

use devkit_net::{TcpReader, TcpStream, TcpWriter};

use crate::link::packet::{HEADER_BYTES, Header, Packet};
use crate::link::transport::{BoxFuture, PacketReader, PacketWriter, Transport};
use crate::{Error, Result};

const BUFFER: usize = 64 * 1024;

/// Connects to adbd at `addr` (`host:port`), as `adb connect` does.
pub async fn connect(addr: &str) -> Result<Box<dyn Transport>> {
    let stream = TcpStream::builder().rx_buffer(BUFFER).tx_buffer(BUFFER).connect_async(addr).await.map_err(Error::Net)?;
    let _ = stream.set_nodelay(true);
    Ok(Box::new(TcpDevice(stream)))
}

struct TcpDevice(TcpStream);

impl Transport for TcpDevice {
    fn split(self: Box<Self>) -> (Box<dyn PacketReader>, Box<dyn PacketWriter>) {
        let (reader, writer) = self.0.split();
        (Box::new(TcpPacketReader(reader)), Box::new(TcpPacketWriter(writer)))
    }
}

fn link(err: devkit_net::Error) -> Error {
    match err {
        devkit_net::Error::NoMemory => Error::NoMemory,
        _ => Error::Disconnected,
    }
}

async fn read_exact(reader: &TcpReader, mut buf: &mut [u8]) -> Result<()> {
    while !buf.is_empty() {
        let n = reader.read_async(buf).await.map_err(link)?;
        if n == 0 {
            return Err(Error::Disconnected);
        }
        buf = &mut buf[n..];
    }
    Ok(())
}

struct TcpPacketReader(TcpReader);

impl TcpPacketReader {
    async fn read_packet(&mut self) -> Result<Packet> {
        let mut head = [0u8; HEADER_BYTES];
        read_exact(&self.0, &mut head).await?;
        let header = Header::parse(&head).ok_or(Error::Protocol)?;
        let mut payload = Vec::new();
        payload.try_reserve_exact(header.length as usize).map_err(|_| Error::NoMemory)?;
        payload.resize(header.length as usize, 0);
        read_exact(&self.0, &mut payload).await?;
        Ok(Packet::new(header.command, header.arg0, header.arg1, payload))
    }
}

impl PacketReader for TcpPacketReader {
    fn read(&mut self) -> BoxFuture<'_, Result<Packet>> {
        Box::pin(self.read_packet())
    }
}

struct TcpPacketWriter(TcpWriter);

impl TcpPacketWriter {
    async fn write_packet(&mut self, packet: &Packet) -> Result<()> {
        self.0.write_all_async(&packet.header()).await.map_err(link)?;
        for segment in packet.payload.segments() {
            self.0.write_all_async(segment).await.map_err(link)?;
        }
        Ok(())
    }
}

impl PacketWriter for TcpPacketWriter {
    fn write<'a>(&'a mut self, packet: &'a Packet) -> BoxFuture<'a, Result<()>> {
        Box::pin(self.write_packet(packet))
    }
}
