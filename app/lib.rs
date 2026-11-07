/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#![cfg_attr(not(test), no_std)]

extern crate alloc;

mod adb_link;
mod agent;
mod device_info;
mod fonts;
mod screens;
mod settings;
mod status_bar;
#[cfg(test)]
mod tests;

mod resources {
    #![allow(dead_code)]
    include!(concat!(env!("OUT_DIR"), "/resources.rs"));
}

use alloc::boxed::Box;
use core::ffi::CStr;

use devkit::StaticLease;
use devkit_ui::{App, AppConfig, AutoRotation, SavedColorMode, StripSource, Strips};
use idf::log::error;

use screens::HomeScreen;
use status_bar::StatusBar;

const TAG: &CStr = c"tab5-adb";
pub(crate) const SHARED_SRAM_BYTES: usize = 245760;
pub(crate) const STRIP_LANES: usize = 2;

#[repr(C, align(64))]
pub(crate) struct SharedSram([u8; SHARED_SRAM_BYTES]);

impl AsMut<[u8]> for SharedSram {
    fn as_mut(&mut self) -> &mut [u8] {
        &mut self.0
    }
}

static SRAM: StaticLease<SharedSram> = StaticLease::new(SharedSram([0; SHARED_SRAM_BYTES]));

pub fn run() {
    settings::init();
    let mut bsp_config = bsp_rs::Config::default();
    bsp_config.display.fb_num = 3;
    bsp_config.display.pixel_format = Some(settings::color_depth().pixel_format());
    let board = match bsp_rs::init(&bsp_config) {
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
    adb_link::start(board.power);
    harness_rs::register("adb", screens::adb_command);
    if let Err(err) = harness_rs::start(&board) {
        error!(TAG, "harness_rs::start: {err}");
    }
    settings::set_display(display);
    let mut config = AppConfig::new(&resources::TOUCH);
    let rotation = AutoRotation::new(board.imu);
    config.plugins.push(Box::new(rotation.clone()));
    config.plugins.push(Box::new(SavedColorMode::new()));
    screens::set_rotation(rotation);
    let Some(sram) = SRAM.take() else {
        error!(TAG, "strips: taken");
        return;
    };
    config.presenter.strips = StripSource::Given(Strips::new(sram));
    config.presenter.lanes = STRIP_LANES;
    let mut app = match App::new(display, board.touch, HomeScreen::new(), config) {
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
