/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Device services, built only on `link::Connection::open` and `link::Stream`.
 */

mod shell;
pub mod sync;

pub use shell::{exec, shell};
