/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::format;
use alloc::vec::Vec;

use crate::Result;
use crate::link::{Connection, Stream};

/// Runs `command` and returns what it printed, stdout and stderr together,
/// once it exits.
pub async fn exec(conn: &Connection, command: &str) -> Result<Vec<u8>> {
    shell(conn, command).await?.read_to_end().await
}

/// A shell running `command`, or an interactive one on a terminal when it is
/// empty. Its output arrives as it is, stdout and stderr together.
pub async fn shell(conn: &Connection, command: &str) -> Result<Stream> {
    conn.open(&format!("shell:{command}")).await
}
