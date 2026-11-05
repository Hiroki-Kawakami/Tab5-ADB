/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#![cfg_attr(not(test), no_std)]

extern crate alloc;

mod fonts;
mod screens;
mod settings;
mod status_bar;

mod resources {
    #![allow(dead_code)]
    include!(concat!(env!("OUT_DIR"), "/resources.rs"));
}

use alloc::boxed::Box;
use core::ffi::CStr;

use devkit_ui::{App, AppConfig, AutoRotation, SavedColorMode};
use idf::log::error;

use screens::Home;
use status_bar::StatusBar;

const TAG: &CStr = c"tab5-adb";

pub fn run() {
    let board = match bsp_rs::init(&bsp_rs::Config::default()) {
        Ok(board) => board,
        Err(err) => {
            error!(TAG, "bsp_rs::init: {err}");
            return;
        }
    };
    let Some(display) = board.display else {
        error!(TAG, "no display");
        return;
    };
    settings::init(Some(display));
    let mut config = AppConfig::new(&resources::TOUCH);
    let rotation = AutoRotation::new(board.imu);
    config.plugins.push(Box::new(rotation.clone()));
    config.plugins.push(Box::new(SavedColorMode::new()));
    screens::set_rotation(rotation);
    let mut app = match App::new(display, board.touch, Home::new(), config) {
        Ok(app) => app,
        Err(err) => {
            error!(TAG, "app: {err}");
            return;
        }
    };
    screens::set_input(app.input());
    app.chrome(StatusBar::new(board.rtc, board.power));
    app.run();
}
