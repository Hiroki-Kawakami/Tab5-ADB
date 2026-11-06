/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#![cfg_attr(not(test), no_std)]

extern crate alloc;

mod error;
pub mod link;
pub mod services;
#[cfg(test)]
mod tests;

pub use error::{Error, Result};
