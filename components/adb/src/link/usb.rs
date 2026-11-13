/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::boxed::Box;
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::ffi::CStr;

use idf::heap::Caps;
use idf::log::error;
use idf::sync::Mutex;
use usb_host_rs::{Buffer, Device, Endpoint, Interface};

use crate::link::packet::{Chunk, HEADER_BYTES, Header, Packet};
use crate::link::transport::{BoxFuture, PacketReader, PacketWriter, Transport};
use crate::{Error, Result};

const TAG: &CStr = c"adb_usb";

const CLASS: u8 = 0xff;
const SUBCLASS: u8 = 0x42;
const PROTOCOL: u8 = 0x01;
/// One bulk transfer's worth of a packet, a multiple of every max packet size.
const CHUNK: usize = 64 * 1024;
const HEADER_CAPS: Caps = Caps::DMA.union(Caps::INTERNAL);
const PAYLOAD_CAPS: Caps = Caps::SPIRAM;
/// Packet buffers kept for reuse once read, so a packet is neither copied
/// nor zeroed again.
const SPARE_BUFFERS: usize = 8;

fn find(device: &Device) -> Option<u8> {
    device
        .configuration()
        .interfaces()
        .find(|i| i.alternate == 0 && (i.class, i.subclass, i.protocol) == (CLASS, SUBCLASS, PROTOCOL))
        .map(|i| i.number)
}

/// Whether `device` has an ADB interface; claims nothing.
pub fn has_interface(device: &Device) -> bool {
    find(device).is_some()
}

/// Claims the ADB interface for a connection. `Busy` while another holds it;
/// the claim ends with the connection. adbd resets its USB on a new claim
/// once a connection has run, so keep one per enumeration.
pub fn open(device: &Device) -> Result<Box<dyn Transport>> {
    let number = find(device).ok_or(Error::Usb(usb_host_rs::Error::NotFound))?;
    let interface = device.claim(number, 0).map_err(Error::Usb)?;
    let input = interface.endpoints().find(|e| e.descriptor().is_in());
    let output = interface.endpoints().find(|e| !e.descriptor().is_in());
    let (Some(input), Some(output)) = (input, output) else {
        error!(TAG, "interface {number} lacks a bulk pair");
        return Err(Error::Usb(usb_host_rs::Error::NotFound));
    };
    Ok(Box::new(UsbDevice { interface, input, output }))
}

struct UsbDevice {
    interface: Interface,
    input: Endpoint,
    output: Endpoint,
}

impl Transport for UsbDevice {
    fn split(self: Box<Self>) -> (Box<dyn PacketReader>, Box<dyn PacketWriter>) {
        let writer = UsbWriter { _interface: self.interface.clone(), output: self.output, unhalt: Some(self.input.clone()), header: None, payload: None };
        let reader = UsbReader { _interface: self.interface, input: self.input, header: None, spare: Arc::new(Mutex::new(Vec::new())) };
        (Box::new(reader), Box::new(writer))
    }
}

fn link(err: usb_host_rs::Error) -> Error {
    error!(TAG, "transfer: {err}");
    Error::Disconnected
}

fn take(slot: &mut Option<Buffer>, len: usize, caps: Caps) -> Result<Buffer> {
    match slot.take() {
        Some(buf) if buf.len() >= len => Ok(buf),
        _ => Buffer::new(len, caps).map_err(|_| Error::NoMemory),
    }
}

struct Received {
    buf: Option<Buffer>,
    len: usize,
    spare: Arc<Mutex<Vec<Buffer>>>,
}

impl AsRef<[u8]> for Received {
    fn as_ref(&self) -> &[u8] {
        self.buf.as_deref().map_or(&[], |b| &b[..self.len])
    }
}

impl Drop for Received {
    fn drop(&mut self) {
        let mut spare = self.spare.lock();
        if spare.len() < SPARE_BUFFERS
            && let Some(buf) = self.buf.take()
        {
            spare.push(buf);
        }
    }
}

struct UsbReader {
    _interface: Interface,
    input: Endpoint,
    header: Option<Buffer>,
    spare: Arc<Mutex<Vec<Buffer>>>,
}

impl UsbReader {
    async fn header(&mut self) -> Result<Header> {
        let mps = usize::from(self.input.max_packet_bytes()).max(1);
        loop {
            let buf = take(&mut self.header, mps, HEADER_CAPS)?;
            let (buf, r) = self.input.transfer(buf, mps).await;
            let n = r.map_err(link)?;
            let parsed = (n == HEADER_BYTES).then(|| Header::parse(buf[..HEADER_BYTES].try_into().unwrap())).flatten();
            self.header = Some(buf);
            match parsed {
                Some(header) => return Ok(header),
                None if n == 0 => continue,
                None => return Err(Error::Protocol),
            }
        }
    }

    async fn payload(&mut self, len: usize) -> Result<Chunk> {
        let mps = usize::from(self.input.max_packet_bytes()).max(1);
        let capacity = len.next_multiple_of(mps);
        let spare = {
            let mut spare = self.spare.lock();
            let at = spare.iter().position(|b| b.len() >= capacity);
            at.map(|at| spare.swap_remove(at))
        };
        let mut buf = match spare {
            Some(buf) => buf,
            None => Buffer::new(capacity, PAYLOAD_CAPS).map_err(|_| Error::NoMemory)?,
        };
        let mut got = 0;
        while got < len {
            let want = (len - got).next_multiple_of(mps).min(CHUNK);
            let (back, r) = self.input.transfer_at(buf, got, want).await;
            buf = back;
            let n = r.map_err(link)?;
            got += n.min(len - got);
            if n < want && got < len {
                return Err(Error::Protocol);
            }
        }
        Ok(Arc::new(Received { buf: Some(buf), len, spare: self.spare.clone() }))
    }
}

impl PacketReader for UsbReader {
    fn read_header(&mut self) -> BoxFuture<'_, Result<Header>> {
        Box::pin(self.header())
    }

    fn read_payload(&mut self, len: usize) -> BoxFuture<'_, Result<Chunk>> {
        Box::pin(self.payload(len))
    }
}

struct UsbWriter {
    _interface: Interface,
    output: Endpoint,
    /// Brings the device's data toggles to DATA0 with the new pipes before the
    /// first packet; adbd takes this only before it has read anything.
    unhalt: Option<Endpoint>,
    header: Option<Buffer>,
    payload: Option<Buffer>,
}

impl UsbWriter {
    async fn write_packet(&mut self, packet: &Packet) -> Result<()> {
        if let Some(input) = self.unhalt.take() {
            input.clear_halt().await.map_err(link)?;
            self.output.clear_halt().await.map_err(link)?;
        }
        let mut buf = take(&mut self.header, HEADER_BYTES, HEADER_CAPS)?;
        buf[..HEADER_BYTES].copy_from_slice(&packet.header());
        let (buf, r) = self.output.transfer(buf, HEADER_BYTES).await;
        self.header = Some(buf);
        r.map_err(link)?;
        let chunks: Vec<&[u8]> = packet.payload.segments().flat_map(|s| s.chunks(CHUNK)).collect();
        let last = chunks.len().saturating_sub(1);
        for (i, chunk) in chunks.into_iter().enumerate() {
            let mut buf = take(&mut self.payload, CHUNK, PAYLOAD_CAPS)?;
            buf[..chunk.len()].copy_from_slice(chunk);
            let (buf, r) = if i == last {
                self.output.transfer_zlp(buf, chunk.len()).await
            } else {
                self.output.transfer(buf, chunk.len()).await
            };
            self.payload = Some(buf);
            r.map_err(link)?;
        }
        Ok(())
    }
}

impl PacketWriter for UsbWriter {
    fn write<'a>(&'a mut self, packet: &'a Packet) -> BoxFuture<'a, Result<()>> {
        Box::pin(self.write_packet(packet))
    }
}
