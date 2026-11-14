/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * The internal RAM the UI and the mirror draw in, and an ADB connection over
 * TCP receives in.
 */

use alloc::boxed::Box;

use adb::link::tcp::{Memory, TLS_RECORD};
use devkit_core::{BufferBytes, BufferLease, BufferPool};
use idf::sync::FreeRtos;

const BYTES: usize = 245760;
const RING_BYTES: usize = 64 * 1024;
const ALIGN: usize = 64;
pub(crate) const UI_BYTES: usize = BYTES - RING_BYTES - TLS_RECORD.next_multiple_of(ALIGN);

static SRAM: BufferPool<BufferBytes<BYTES>, 3, FreeRtos> = BufferPool::new(BufferBytes::new());

pub(crate) type Sram = BufferLease<'static>;

pub(crate) fn ui() -> Option<Sram> {
    SRAM.alloc(UI_BYTES, ALIGN)
}

/// All of it, or what a connection leaves.
pub(crate) fn mirror() -> Option<Sram> {
    SRAM.alloc(BYTES, ALIGN).or_else(ui)
}

pub(crate) fn tcp() -> Option<Memory> {
    let ring = SRAM.alloc(RING_BYTES, ALIGN)?;
    let record = SRAM.alloc(TLS_RECORD, ALIGN)?;
    Some(Memory { ring: Box::new(ring), record: Box::new(record) })
}
