/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

mod adb_dashboard;
mod connecting;
mod home;
mod message;
mod mirror;
mod settings;

pub use home::{HomeScreen, adb_command};
pub use settings::{set_input, set_rotation};
