/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

mod adb_dashboard;
mod apk_preview;
mod apps;
mod connecting;
mod device_info;
mod file_dialogs;
mod file_manager;
mod file_preview;
mod home;
mod image_preview;
mod logcat;
mod message;
pub(crate) mod mirror;
mod now_playing;
mod power_menu;
mod screenshot;
mod settings;
mod terminal;

pub use home::{HomeScreen, adb_command};
pub use settings::{set_input, set_rotation};
