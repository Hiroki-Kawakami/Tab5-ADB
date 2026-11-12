/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::boxed::Box;
use alloc::sync::Arc;
use alloc::vec::Vec;

use devkit_crypto::tls::{ClientConfig, Verify};
use devkit_net::{TcpStream, TlsReader, TlsStream, TlsWriter};
use idf::heap::Caps;
use idf::sync::Mutex;

use crate::link::identity::Identity;
use crate::link::packet::{Chunk, HEADER_BYTES, Header, Packet, Payload};
use crate::link::transport::{BoxFuture, PacketReader, PacketWriter, Transport};
use crate::{Error, Result};

const BUFFER: usize = 64 * 1024;
const COALESCE: usize = 4 * 1024;
const PAYLOAD_CAPS: Caps = Caps::SPIRAM;

/// Connects to adbd at `addr` (`host:port`), as `adb connect` does.
pub async fn connect(addr: &str) -> Result<Box<dyn Transport>> {
    let stream = TcpStream::builder().rx_buffer(BUFFER).tx_buffer(BUFFER).connect_async(addr).await.map_err(Error::Net)?;
    let _ = stream.set_nodelay(true);
    Ok(Box::new(TcpDevice(stream)))
}

struct TcpDevice(TcpStream);

impl Transport for TcpDevice {
    fn split(self: Box<Self>) -> (Box<dyn PacketReader>, Box<dyn PacketWriter>) {
        let stream = Arc::new(self.0);
        let write = Arc::new(Mutex::new(WriteHalf::Plain(stream.clone())));
        (Box::new(TcpPacketReader { read: ReadHalf::Plain(stream), write: write.clone() }), Box::new(TcpPacketWriter(write)))
    }
}

fn link(err: devkit_net::Error) -> Error {
    match err {
        devkit_net::Error::Tls(e) => Error::Crypto(e),
        devkit_net::Error::NoMemory => Error::NoMemory,
        _ => Error::Disconnected,
    }
}

enum ReadHalf {
    Plain(Arc<TcpStream>),
    Tls(TlsReader),
    Gone,
}

enum WriteHalf {
    Plain(Arc<TcpStream>),
    Tls(TlsWriter),
    Gone,
}

struct TcpPacketReader {
    read: ReadHalf,
    write: Arc<Mutex<WriteHalf>>,
}

impl TcpPacketReader {
    async fn read_exact(&mut self, mut buf: &mut [u8]) -> Result<()> {
        while !buf.is_empty() {
            let n = match &mut self.read {
                ReadHalf::Plain(tcp) => tcp.read_async(buf).await,
                ReadHalf::Tls(tls) => tls.read_async(buf).await,
                ReadHalf::Gone => return Err(Error::Disconnected),
            };
            let n = n.map_err(link)?;
            if n == 0 {
                return Err(Error::Disconnected);
            }
            buf = &mut buf[n..];
        }
        Ok(())
    }

    async fn read_packet(&mut self) -> Result<Packet> {
        let mut head = [0u8; HEADER_BYTES];
        self.read_exact(&mut head).await?;
        let header = Header::parse(&head).ok_or(Error::Protocol)?;
        let len = header.length as usize;
        if len == 0 {
            return Ok(Packet::new(header.command, header.arg0, header.arg1, Vec::new()));
        }
        let mut payload = idf::heap::try_vec(len, PAYLOAD_CAPS).map_err(|_| Error::NoMemory)?;
        payload.resize(len, 0);
        self.read_exact(&mut payload).await?;
        let chunk: Chunk = Arc::new(payload);
        Ok(Packet { command: header.command, arg0: header.arg0, arg1: header.arg1, payload: Payload::Chunks(alloc::vec![chunk]) })
    }

    async fn upgrade(&mut self, identity: &Identity) -> Result<()> {
        let mut write = self.write.lock_async().await;
        let (WriteHalf::Plain(_), ReadHalf::Plain(_)) = (&*write, &self.read) else { return Err(Error::Protocol) };
        *write = WriteHalf::Gone;
        let ReadHalf::Plain(stream) = core::mem::replace(&mut self.read, ReadHalf::Gone) else { unreachable!() };
        let tcp = Arc::into_inner(stream).ok_or(Error::Protocol)?;
        let mut config = ClientConfig::new(Verify::None);
        config.identity = Some(identity.tls_identity()?);
        // DEFAULT lands in PSRAM ahead of RETENT_RAM on the P4, where AES-GCM
        // opens records at a tenth of the speed.
        config.buffer_caps = Caps::INTERNAL;
        let tls = TlsStream::start_async(tcp, None, Arc::new(config)).await.map_err(link)?;
        let (reader, writer) = tls.split();
        self.read = ReadHalf::Tls(reader);
        *write = WriteHalf::Tls(writer);
        Ok(())
    }
}

impl PacketReader for TcpPacketReader {
    fn read(&mut self) -> BoxFuture<'_, Result<Packet>> {
        Box::pin(self.read_packet())
    }

    fn start_tls<'a>(&'a mut self, identity: &'a Identity) -> BoxFuture<'a, Result<()>> {
        Box::pin(self.upgrade(identity))
    }
}

struct TcpPacketWriter(Arc<Mutex<WriteHalf>>);

impl TcpPacketWriter {
    async fn write_packet(&mut self, packet: &Packet) -> Result<()> {
        let mut write = self.0.lock_async().await;
        let header = packet.header();
        match &mut *write {
            WriteHalf::Plain(tcp) => {
                tcp.write_all_async(&header).await.map_err(link)?;
                for segment in packet.payload.segments() {
                    tcp.write_all_async(segment).await.map_err(link)?;
                }
            }
            WriteHalf::Tls(tls) if packet.payload.len() <= COALESCE => {
                let mut record = Vec::new();
                record.try_reserve_exact(HEADER_BYTES + packet.payload.len()).map_err(|_| Error::NoMemory)?;
                record.extend_from_slice(&header);
                packet.payload.segments().for_each(|segment| record.extend_from_slice(segment));
                tls.write_all_async(&record).await.map_err(link)?;
            }
            WriteHalf::Tls(tls) => {
                tls.write_all_async(&header).await.map_err(link)?;
                for segment in packet.payload.segments() {
                    tls.write_all_async(segment).await.map_err(link)?;
                }
            }
            WriteHalf::Gone => return Err(Error::Disconnected),
        }
        Ok(())
    }
}

impl PacketWriter for TcpPacketWriter {
    fn write<'a>(&'a mut self, packet: &'a Packet) -> BoxFuture<'a, Result<()>> {
        Box::pin(self.write_packet(packet))
    }
}
