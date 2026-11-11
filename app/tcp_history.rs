/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Recent ADB-over-TCP addresses, newest first.
 */

use alloc::string::String;
use alloc::vec::Vec;
use core::ffi::CStr;

use idf::log::error;
use idf::nvs::{Key, Nvs};

const TAG: &CStr = c"tcp_history";
const NAMESPACE: &CStr = c"tab5adb";
const KEY: Key<String> = Key::new(c"tcp_history");
const CAP: usize = 10;

pub fn load() -> Vec<String> {
    open().map(|nvs| read(&nvs)).unwrap_or_default()
}

/// Moves `addr` to the front, keeping at most `CAP`.
pub fn add(addr: &str) {
    let addr = addr.trim();
    if addr.is_empty() {
        return;
    }
    let Some(mut nvs) = open() else { return };
    let mut list = read(&nvs);
    list.retain(|a| a != addr);
    list.insert(0, addr.into());
    list.truncate(CAP);
    if let Err(err) = nvs.set(KEY, &list.join("\n")).and_then(|()| nvs.commit()) {
        error!(TAG, "set: {err}");
    }
}

fn open() -> Option<Nvs> {
    Nvs::open(NAMESPACE).inspect_err(|err| error!(TAG, "open: {err}")).ok()
}

fn read(nvs: &Nvs) -> Vec<String> {
    match nvs.get(KEY) {
        Ok(value) => value.unwrap_or_default().lines().filter(|l| !l.is_empty()).map(String::from).collect(),
        Err(err) => {
            error!(TAG, "get: {err}");
            Vec::new()
        }
    }
}
