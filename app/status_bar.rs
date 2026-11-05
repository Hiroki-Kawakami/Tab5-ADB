/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use core::ffi::CStr;
use core::time::Duration;

use bsp_rs::power::Power;
use bsp_rs::rtc::Rtc;
use devkit_ui::{Align, Bars, Component, Cx, Element, This, Ui, id};
use idf::executor;
use idf::log::error;

use crate::fonts::small_icon;
use crate::resources::icons;

const TAG: &CStr = c"status_bar";

pub const STATUS_BAR_HEIGHT: i16 = 52;

pub struct StatusBar {
    rtc: Option<&'static Rtc>,
    power: &'static Power,
    time: Option<(u8, u8)>,
    battery: Option<u8>,
}

impl StatusBar {
    pub fn new(rtc: Option<&'static Rtc>, power: &'static Power) -> Self {
        Self { rtc, power, time: None, battery: None }
    }
}

async fn poll(this: This<StatusBar>, rtc: Option<&'static Rtc>, power: &'static Power) {
    loop {
        let now = rtc.and_then(|rtc| rtc.now().inspect_err(|err| error!(TAG, "rtc.now: {err}")).ok());
        let battery = power.read().inspect_err(|err| error!(TAG, "power.read: {err}")).ok().and_then(|state| state.battery);
        let alive = this.with(|s, cx| {
            s.time = now.map(|now| (now.hour, now.minute));
            s.battery = battery.map(|b| b.level);
            cx.rebuild();
        });
        if alive.is_none() {
            return;
        }
        let second = now.map_or(0, |now| now.second.min(59));
        executor::sleep(Duration::from_secs(u64::from(60 - second))).await;
    }
}

impl Component for StatusBar {
    type Msg = ();

    fn update(&mut self, _: (), _: &mut Cx<Self>) {}

    fn view<'a>(&'a self, ui: &'a Ui<()>) -> Element<'a, ()> {
        if ui.screen_style().bars == Bars::Hidden {
            return ui.empty();
        }
        let (rtc, power) = (self.rtc, self.power);
        let color = ui.colors().text;
        let caption = ui.theme().fonts.caption;
        let time = match self.time {
            Some((h, m)) => ui.format(format_args!("{h:02}:{m:02}")),
            None => "",
        };
        let battery = match self.battery {
            Some(level) => ui.row([
                ui.icon(small_icon(icons::BATTERY)).foreground(color),
                ui.text(caption, ui.format(format_args!("{level}%"))).foreground(color),
            ])
            .gap(14)
            .align(Align::Center),
            None => ui.empty(),
        };
        ui.row([
            ui.text(caption, time).foreground(color).id(id!("clock")).grow(1),
            ui.icon(small_icon(icons::WIFI)).foreground(color),
            battery,
        ])
        .height(STATUS_BAR_HEIGHT)
        .padding_x(24)
        .gap(14)
        .align(Align::Center)
        .task(move |this| poll(this, rtc, power))
    }
}
