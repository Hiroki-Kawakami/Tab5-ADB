/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use core::ffi::CStr;
use core::time::Duration;

use bsp_rs::power::{Battery, Power};
use devkit_gfx::Color;
use devkit_net::wifi_manager::State;
use devkit_ui::{Align, Bars, Component, Cx, Element, This, Ui, id};
use idf::executor;
use idf::log::error;

use crate::fonts::small_icon;
use crate::resources::icons;
use crate::{clock, wifi};

const TAG: &CStr = c"status_bar";

pub const STATUS_BAR_HEIGHT: i16 = 52;
const UNSET_CLOCK_POLL_SECS: u64 = 5;
const BATTERY_POLL_SECS: u64 = 15;
const FAINT_OPACITY: u8 = 80;

pub struct StatusBar {
    power: &'static Power,
    time: Option<(u8, u8)>,
    battery: Battery,
}

impl StatusBar {
    pub fn new(power: &'static Power) -> Self {
        Self { power, time: None, battery: Battery::Unknown }
    }
}

async fn poll(this: This<StatusBar>, power: &'static Power) {
    loop {
        let now = clock::local_now();
        let battery = power.read().inspect_err(|err| error!(TAG, "power.read: {err}")).map_or(Battery::Unknown, |state| state.battery);
        let alive = this.with(|s, cx| {
            s.time = now.map(|now| (now.hour, now.minute));
            s.battery = battery;
            cx.rebuild();
        });
        if alive.is_none() {
            return;
        }
        let wait = now.map_or(UNSET_CLOCK_POLL_SECS, |now| 60 - u64::from(now.second.min(59))).min(BATTERY_POLL_SECS);
        executor::sleep(Duration::from_secs(wait)).await;
    }
}

impl Component for StatusBar {
    type Msg = ();

    fn update(&mut self, _: (), _: &mut Cx<Self>) {}

    fn view<'a>(&'a self, ui: &'a Ui<()>) -> Element<'a, ()> {
        if ui.screen_style().bars == Bars::Hidden {
            return ui.empty();
        }
        let power = self.power;
        let color = ui.colors().text;
        let caption = ui.theme().fonts.caption;
        let time = match self.time {
            Some((h, m)) => ui.format(format_args!("{h:02}:{m:02}")),
            None => "--:--",
        };
        let battery = match self.battery {
            Battery::Present(b) => {
                let glyph = if b.charging == Some(true) { icons::BATTERY_CHARGING } else { level_glyph(b.level) };
                ui.row([
                    ui.icon(small_icon(glyph)).foreground(color),
                    ui.text(caption, ui.format(format_args!("{}%", b.level))).foreground(color),
                ])
                .gap(14)
                .align(Align::Center)
            }
            Battery::Absent => ui.icon(small_icon(icons::BATTERY_OFF)).foreground(color),
            Battery::Unknown => ui.empty(),
        };
        ui.row([
            ui.text(caption, time).foreground(color).id(id!("clock")).grow(1),
            wifi_icon(ui, color).task(|this| wifi::follow(this, |_: &mut StatusBar| {})),
            battery,
        ])
        .height(STATUS_BAR_HEIGHT)
        .padding_x(24)
        .gap(14)
        .align(Align::Center)
        .task(move |this| poll(this, power))
    }
}

fn level_glyph(level: u8) -> char {
    match level {
        0..=10 => icons::BATTERY_0,
        11..=35 => icons::BATTERY_1,
        36..=60 => icons::BATTERY_2,
        61..=85 => icons::BATTERY,
        _ => icons::BATTERY_4,
    }
}

fn wifi_icon<'a>(ui: &'a Ui<()>, color: Color) -> Element<'a, ()> {
    let faint = ui.icon(small_icon(icons::WIFI)).foreground(color.with_opacity(FAINT_OPACITY));
    match wifi::state() {
        State::Disabled => ui.icon(small_icon(icons::WIFI_OFF)).foreground(color),
        State::Connected { rssi, .. } => {
            ui.column([faint, ui.icon(small_icon(wifi::strength_glyph(rssi))).foreground(color).absolute(Some(0), Some(0), None, None)])
        }
        _ => faint,
    }
}
