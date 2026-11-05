/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#![cfg_attr(not(test), no_std)]

extern crate alloc;

mod screens;
mod status_bar;

mod resources {
    #![allow(dead_code)]
    include!(concat!(env!("OUT_DIR"), "/resources.rs"));
}

use core::ffi::CStr;

use devkit_ui::{App, AppConfig};
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
    if let Err(err) = display.set_brightness(100) {
        error!(TAG, "set_brightness: {err}");
    }
    let mut app = match App::new(display, board.touch, Home, AppConfig::new(&resources::TOUCH)) {
        Ok(app) => app,
        Err(err) => {
            error!(TAG, "app: {err}");
            return;
        }
    };
    app.chrome(StatusBar::new(board.rtc, board.power));
    app.run();
}

