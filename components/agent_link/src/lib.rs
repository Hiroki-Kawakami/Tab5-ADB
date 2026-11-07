/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * The Tab5 end of android-agent/docs/protocol.md.
 */

#![cfg_attr(not(test), no_std)]

extern crate alloc;

mod error;
mod link;
pub mod protocol;
#[cfg(test)]
mod tests;

pub use error::{Error, Result};
pub use link::{AgentInfo, HelloConfig, Link};
