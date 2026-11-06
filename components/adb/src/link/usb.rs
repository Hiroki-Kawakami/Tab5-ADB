/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::ffi::CStr;

use idf::heap::Caps;
use idf::log::error;
use usb_host_rs::{Buffer, Device, Endpoint, Interface};

use crate::link::packet::{HEADER_BYTES, Header, Packet};
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
/// the claim ends with the connection, so the device opens again after.
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
        let reader = UsbReader { _interface: self.interface.clone(), input: self.input, header: None, payload: None };
        let writer = UsbWriter { _interface: self.interface, output: self.output, header: None, payload: None };
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

struct UsbReader {
    _interface: Interface,
    input: Endpoint,
    header: Option<Buffer>,
    payload: Option<Buffer>,
}

impl UsbReader {
    async fn read_packet(&mut self) -> Result<Packet> {
        let mps = usize::from(self.input.max_packet_bytes()).max(1);
        let header = loop {
            let buf = take(&mut self.header, mps, HEADER_CAPS)?;
            let (buf, r) = self.input.transfer(buf, mps).await;
            let n = r.map_err(link)?;
            let parsed = (n == HEADER_BYTES).then(|| Header::parse(buf[..HEADER_BYTES].try_into().unwrap())).flatten();
            self.header = Some(buf);
            match parsed {
                Some(header) => break header,
                None if n == 0 => continue,
                None => return Err(Error::Protocol),
            }
        };
        let len = header.length as usize;
        let mut payload = Vec::new();
        payload.try_reserve_exact(len).map_err(|_| Error::NoMemory)?;
        while payload.len() < len {
            let want = (len - payload.len()).next_multiple_of(mps).min(CHUNK);
            let buf = take(&mut self.payload, CHUNK, PAYLOAD_CAPS)?;
            let (buf, r) = self.input.transfer(buf, want).await;
            let n = r.map_err(link)?;
            payload.extend_from_slice(&buf[..n.min(len - payload.len())]);
            self.payload = Some(buf);
            if n < want && payload.len() < len {
                return Err(Error::Protocol);
            }
        }
        Ok(Packet::new(header.command, header.arg0, header.arg1, payload))
    }
}

impl PacketReader for UsbReader {
    fn read(&mut self) -> BoxFuture<'_, Result<Packet>> {
        Box::pin(self.read_packet())
    }
}

struct UsbWriter {
    _interface: Interface,
    output: Endpoint,
    header: Option<Buffer>,
    payload: Option<Buffer>,
}

impl UsbWriter {
    async fn write_packet(&mut self, packet: &Packet) -> Result<()> {
        let mut buf = take(&mut self.header, HEADER_BYTES, HEADER_CAPS)?;
        buf[..HEADER_BYTES].copy_from_slice(&packet.header());
        let (buf, r) = self.output.transfer(buf, HEADER_BYTES).await;
        self.header = Some(buf);
        r.map_err(link)?;
        let chunks = packet.payload.chunks(CHUNK);
        let last = chunks.len().saturating_sub(1);
        for (i, chunk) in chunks.enumerate() {
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
