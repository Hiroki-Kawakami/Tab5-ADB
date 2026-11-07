/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::String;
use alloc::vec::Vec;
use core::ffi::CStr;

use devkit_crypto::hash::HashAlg;
use devkit_crypto::rsa::PrivateKey;
use idf::nvs::{Key, Nvs};

use crate::link::base64;
use crate::{Error, Result};

const KEY_BITS: usize = 2048;
const WORDS: usize = KEY_BITS / 32;
/// The android_pubkey RSAPublicKey struct: length, n0inv, modulus, rr, exponent.
const PUBLIC_KEY_BYTES: usize = 4 + 4 + KEY_BITS / 8 * 2 + 4;
const TOKEN_BYTES: usize = 20;
const PRIVATE_KEY: Key<Vec<u8>> = Key::new(c"privkey");

/// The RSA key this host authenticates with. A device that was told to
/// always allow it skips the dialog, which shows the key's `name`.
pub struct Identity {
    key: PrivateKey,
    public: Vec<u8>,
    name: String,
}

impl Identity {
    /// Takes seconds on the device.
    pub fn generate(name: &str) -> Result<Self> {
        Self::new(PrivateKey::generate(KEY_BITS)?, name)
    }

    /// A PKCS #1 RSAPrivateKey.
    pub fn from_der(der: &[u8], name: &str) -> Result<Self> {
        Self::new(PrivateKey::from_pkcs1_der(der)?, name)
    }

    pub fn to_der(&self) -> Result<Vec<u8>> {
        Ok(self.key.to_pkcs1_der()?)
    }

    /// The key kept in NVS under `namespace`, generated and stored the first
    /// time or when what is stored does not parse. `name` is not stored.
    pub fn load_or_create(namespace: &CStr, name: &str) -> Result<Self> {
        let mut nvs = Nvs::open(namespace).map_err(Error::Storage)?;
        if let Some(der) = nvs.get(PRIVATE_KEY).map_err(Error::Storage)?
            && let Ok(identity) = Self::from_der(&der, name)
        {
            return Ok(identity);
        }
        let identity = Self::generate(name)?;
        nvs.set(PRIVATE_KEY, &identity.to_der()?).map_err(Error::Storage)?;
        nvs.commit().map_err(Error::Storage)?;
        Ok(identity)
    }

    fn new(key: PrivateKey, name: &str) -> Result<Self> {
        let public = key.public_key()?;
        let blob = android_public_key(public.modulus(), public.exponent()).ok_or(Error::Crypto(devkit_crypto::Error::InvalidKey))?;
        Ok(Self { key, public: base64::encode(&blob), name: name.into() })
    }

    /// The token signed as if it were a SHA-1 digest, as adbd checks it.
    pub(crate) fn sign_token(&self, token: &[u8]) -> Result<Vec<u8>> {
        if token.len() != TOKEN_BYTES {
            return Err(Error::Protocol);
        }
        Ok(self.key.sign_pkcs1v15_prehashed(HashAlg::Sha1, token)?)
    }

    /// The AUTH RSAPUBLICKEY payload.
    pub(crate) fn public_key_payload(&self) -> Vec<u8> {
        let mut out = self.public.clone();
        out.push(b' ');
        out.extend_from_slice(self.name.as_bytes());
        out.push(0);
        out
    }
}

/// Little-endian words of a big-endian number, `None` past `words`.
fn to_words(be: &[u8], words: usize) -> Option<Vec<u32>> {
    let be = &be[be.iter().position(|&b| b != 0).unwrap_or(be.len())..];
    if be.len() > words * 4 {
        return None;
    }
    let mut out = alloc::vec![0u32; words];
    for (i, &b) in be.iter().rev().enumerate() {
        out[i / 4] |= u32::from(b) << (8 * (i % 4));
    }
    Some(out)
}

/// R² mod n with R = 2^(32 * n.len()), by doubling.
pub(crate) fn r_squared(n: &[u32]) -> Vec<u32> {
    let len = n.len();
    let mut x = alloc::vec![0u32; len + 1];
    x[0] = 1;
    let n_ext: Vec<u32> = n.iter().copied().chain([0]).collect();
    for _ in 0..64 * len {
        let mut carry = 0;
        for w in x.iter_mut() {
            let next = *w >> 31;
            *w = *w << 1 | carry;
            carry = next;
        }
        if ge(&x, &n_ext) {
            let mut borrow = 0u64;
            for (w, &m) in x.iter_mut().zip(&n_ext) {
                let d = u64::from(*w).wrapping_sub(u64::from(m)).wrapping_sub(borrow);
                *w = d as u32;
                borrow = d >> 63;
            }
        }
    }
    x.truncate(len);
    x
}

fn ge(a: &[u32], b: &[u32]) -> bool {
    for (x, y) in a.iter().rev().zip(b.iter().rev()) {
        if x != y {
            return x > y;
        }
    }
    true
}

/// -1/n0 mod 2^32, for odd `n0`.
pub(crate) fn n0inv(n0: u32) -> u32 {
    let mut inv = n0;
    for _ in 0..5 {
        inv = inv.wrapping_mul(2u32.wrapping_sub(n0.wrapping_mul(inv)));
    }
    inv.wrapping_neg()
}

fn android_public_key(modulus: &[u8], exponent: &[u8]) -> Option<[u8; PUBLIC_KEY_BYTES]> {
    let n = to_words(modulus, WORDS)?;
    let e = to_words(exponent, 1)?[0];
    let rr = r_squared(&n);
    let mut out = [0u8; PUBLIC_KEY_BYTES];
    let words = [WORDS as u32, n0inv(n[0])].into_iter().chain(n.iter().copied()).chain(rr).chain([e]);
    for (i, w) in words.enumerate() {
        out[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
    }
    Some(out)
}
