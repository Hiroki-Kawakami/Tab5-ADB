/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::String;
use alloc::sync::Arc;
use alloc::vec;
use alloc::vec::Vec;

use devkit_crypto::aead::{Aead, Algorithm, NONCE, TAG};
use devkit_crypto::hash::Sha256;
use devkit_crypto::kdf::Hkdf;
use devkit_crypto::tls::{ClientConfig, Verify};
use devkit_net::{TcpStream, TlsStream};

use crate::link::identity::Identity;
use crate::link::spake2::{Role, Spake2};
use crate::{Error, Result};

const VERSION: u8 = 1;
const SPAKE2_MSG: u8 = 0;
const PEER_INFO: u8 = 1;
const HEADER_BYTES: usize = 6;
const PEER_INFO_BYTES: usize = 8192;
const MAX_PAYLOAD: usize = 2 * PEER_INFO_BYTES;
const RSA_PUB_KEY: u8 = 0;
const DEVICE_GUID: u8 = 1;
const EXPORTER_LABEL: &[u8] = b"adb-label\0";
const EXPORTED_BYTES: usize = 64;
const KEY_INFO: &[u8] = b"adb pairing_auth aes-128-gcm key";

/// Pairs with the device whose Wireless debugging dialog shows `code` and
/// `addr`, the pairing port rather than the one to connect to; the device
/// accepts `identity` from then on. Returns the device's GUID.
pub async fn pair(addr: &str, code: &str, identity: &Identity) -> Result<String> {
    let tcp = TcpStream::connect_async(addr).await.map_err(Error::Net)?;
    let mut config = ClientConfig::new(Verify::None);
    config.identity = Some(identity.tls_identity()?);
    let mut tls = TlsStream::start_async(tcp, None, Arc::new(config)).await.map_err(link)?;

    let mut password = Vec::from(code.as_bytes());
    password.resize(code.len() + EXPORTED_BYTES, 0);
    tls.connection().export_keying_material(EXPORTER_LABEL, &[], &mut password[code.len()..])?;
    let mut random = [0u8; 64];
    devkit_crypto::rand::fill(&mut random);
    let spake2 = Spake2::new(Role::Alice, &password, &random);

    write_packet(&mut tls, SPAKE2_MSG, spake2.message()).await?;
    let theirs: [u8; 32] = read_packet(&mut tls, SPAKE2_MSG).await?.try_into().map_err(|_| Error::Protocol)?;
    let mut cipher = Cipher::new(&spake2.finish(&theirs).ok_or(Error::Protocol)?)?;

    let mut info = vec![0u8; PEER_INFO_BYTES + TAG];
    info[0] = RSA_PUB_KEY;
    let key = identity.public_key_payload();
    let n = key.len().min(PEER_INFO_BYTES - 2);
    info[1..1 + n].copy_from_slice(&key[..n]);
    cipher.seal(&mut info)?;
    write_packet(&mut tls, PEER_INFO, &info).await?;
    let mut theirs = read_packet(&mut tls, PEER_INFO).await?;
    let info = cipher.open(&mut theirs).map_err(|_| Error::PairingCode)?;
    if info.len() != PEER_INFO_BYTES || info[0] != DEVICE_GUID {
        return Err(Error::Protocol);
    }
    let guid = &info[1..];
    let guid = &guid[..guid.iter().position(|&b| b == 0).unwrap_or(guid.len())];
    let _ = tls.shutdown_async().await;
    Ok(String::from_utf8_lossy(guid).into())
}

fn link(err: devkit_net::Error) -> Error {
    match err {
        devkit_net::Error::Tls(e) => Error::Crypto(e),
        devkit_net::Error::NoMemory => Error::NoMemory,
        _ => Error::Disconnected,
    }
}

async fn write_packet(tls: &mut TlsStream, kind: u8, payload: &[u8]) -> Result<()> {
    let mut packet = Vec::with_capacity(HEADER_BYTES + payload.len());
    packet.extend_from_slice(&[VERSION, kind]);
    packet.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    packet.extend_from_slice(payload);
    tls.write_all_async(&packet).await.map_err(link)
}

async fn read_packet(tls: &mut TlsStream, kind: u8) -> Result<Vec<u8>> {
    let mut header = [0u8; HEADER_BYTES];
    read_exact(tls, &mut header).await?;
    let len = u32::from_be_bytes(header[2..].try_into().unwrap()) as usize;
    if header[0] != VERSION || header[1] != kind || len > MAX_PAYLOAD {
        return Err(Error::Protocol);
    }
    let mut payload = vec![0u8; len];
    read_exact(tls, &mut payload).await?;
    Ok(payload)
}

async fn read_exact(tls: &mut TlsStream, mut buf: &mut [u8]) -> Result<()> {
    while !buf.is_empty() {
        let n = tls.read_async(buf).await.map_err(link)?;
        if n == 0 {
            return Err(Error::Disconnected);
        }
        buf = &mut buf[n..];
    }
    Ok(())
}

/// AES-128-GCM under a key from the SPAKE2 key. Both directions count their
/// nonces from zero under the same key, as adbd does.
pub(crate) struct Cipher {
    aead: Aead,
    sealed: u64,
    opened: u64,
}

impl Cipher {
    pub(crate) fn new(key_material: &[u8]) -> Result<Self> {
        let mut key = [0u8; 16];
        Hkdf::<Sha256>::new(&[], key_material).expand(&[KEY_INFO], &mut key)?;
        Ok(Self { aead: Aead::new(Algorithm::Aes128Gcm, &key)?, sealed: 0, opened: 0 })
    }

    /// Encrypts all but the last `TAG` bytes of `buf`, the tag going there.
    pub(crate) fn seal(&mut self, buf: &mut [u8]) -> Result<()> {
        let len = buf.len().checked_sub(TAG).ok_or(Error::Protocol)?;
        self.aead.seal_in_place(&nonce(self.sealed), &[], buf, len)?;
        self.sealed += 1;
        Ok(())
    }

    pub(crate) fn open<'a>(&mut self, buf: &'a mut [u8]) -> Result<&'a mut [u8]> {
        let plain = self.aead.open_in_place(&nonce(self.opened), &[], buf)?;
        self.opened += 1;
        Ok(plain)
    }
}

fn nonce(sequence: u64) -> [u8; NONCE] {
    let mut nonce = [0u8; NONCE];
    nonce[..8].copy_from_slice(&sequence.to_le_bytes());
    nonce
}
