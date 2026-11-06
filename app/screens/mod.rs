/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

mod adb_dashboard;
mod connecting;
mod home;
mod message;
mod settings;

pub use home::HomeScreen;
pub use settings::{set_input, set_rotation};
