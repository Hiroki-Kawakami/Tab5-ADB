/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::vec;
use alloc::vec::Vec;
use core::ffi::CStr;

use devkit_ui::widgets::{button, divider, modal_from, modal_title};
use devkit_ui::{Align, ColorRole, Component, Cx, Element, Handle, Overlay, Prop, Screen, State, StateStyle, Ui};
use idf::log::error;

use crate::fonts::icon;
use crate::resources::{colors, icons};

use super::adb_dashboard::Msg;
use super::file_dialogs::{Action, Choice};

const TAG: &CStr = c"power_menu";
const WIDTH: i16 = 880;
const ITEM_HEIGHT: i16 = 80;
const CANCEL_HEIGHT: i16 = 78;

static ITEM: StateStyle = StateStyle {
    font: None,
    base: &[Prop::Background(ColorRole::Background), Prop::Radius(14)],
    states: &[(State::PRESSED, &[Prop::Background(colors::PRESSED)])],
};

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum PowerAction {
    PowerOff,
    Restart,
    Recovery,
    Bootloader,
    Sleep,
    Wake,
}

impl PowerAction {
    pub fn command(self) -> &'static str {
        match self {
            Self::PowerOff => "reboot -p",
            Self::Restart => "reboot",
            Self::Recovery => "reboot recovery",
            Self::Bootloader => "reboot bootloader",
            Self::Sleep => "input keyevent 223",
            Self::Wake => "input keyevent 224",
        }
    }

    fn label(self) -> &'static str {
        match self {
            Self::PowerOff => "Power off",
            Self::Restart => "Restart",
            Self::Recovery => "Reboot to Recovery",
            Self::Bootloader => "Reboot to Bootloader",
            Self::Sleep => "Sleep (screen off)",
            Self::Wake => "Wake (screen on)",
        }
    }

    fn glyph(self) -> char {
        match self {
            Self::PowerOff => icons::POWER,
            Self::Restart => icons::REFRESH,
            Self::Recovery => icons::TOOL,
            Self::Bootloader => icons::CPU,
            Self::Sleep => icons::MOON,
            Self::Wake => icons::SUN,
        }
    }

    fn confirmation(self) -> Option<&'static str> {
        match self {
            Self::PowerOff => Some("Power the device off now? The ADB connection will drop."),
            Self::Restart => Some("Reboot the device now? The ADB connection will drop."),
            Self::Recovery => Some("Reboot the device into recovery? The ADB connection will drop."),
            Self::Bootloader => Some("Reboot the device into the bootloader? The ADB connection will drop."),
            Self::Sleep | Self::Wake => None,
        }
    }
}

const REBOOTS: [PowerAction; 4] = [PowerAction::PowerOff, PowerAction::Restart, PowerAction::Recovery, PowerAction::Bootloader];
const SCREEN: [PowerAction; 2] = [PowerAction::Sleep, PowerAction::Wake];

#[derive(Clone, Copy)]
pub enum MenuMsg {
    Pick(PowerAction),
    Cancel,
}

pub struct PowerMenuScreen {
    target: Handle<Msg>,
}

impl PowerMenuScreen {
    pub fn new(target: Handle<Msg>) -> Self {
        Self { target }
    }
}

impl Component for PowerMenuScreen {
    type Msg = MenuMsg;

    fn update(&mut self, msg: MenuMsg, cx: &mut Cx<Self>) {
        match msg {
            MenuMsg::Cancel => cx.nav_of::<Overlay>().pop(),
            MenuMsg::Pick(action) => match action.confirmation() {
                Some(message) => {
                    let label = action.label();
                    let buttons = vec![("Cancel", Action::Secondary, None), (label, Action::Destructive, Some(Msg::Power(action)))];
                    cx.nav_of::<Overlay>().replace(Choice::new(label, message.into(), buttons, self.target.clone()));
                }
                None => {
                    cx.nav_of::<Overlay>().pop();
                    if !self.target.post(Msg::Power(action)) {
                        error!(TAG, "power menu: UI queue full");
                    }
                }
            },
        }
    }

    fn view<'a>(&'a self, ui: &'a Ui<MenuMsg>) -> Element<'a, MenuMsg> {
        let (width, height) = ui.screen_size();
        let columns = if width > height { 2 } else { 1 };
        let gap = ui.theme().widgets.unit.gap;
        let group = |actions: &[PowerAction]| ui.grid(columns, actions.iter().map(|&a| item(ui, a)).collect::<Vec<_>>()).gap(gap);
        let cancel = button(ui, "Cancel", ui.theme().widgets.button.secondary, MenuMsg::Cancel).height(CANCEL_HEIGHT);
        modal_from(ui, ui.column([modal_title(ui, "Power Menu"), group(&REBOOTS), divider(ui), group(&SCREEN), cancel]).width(WIDTH))
    }
}

impl Screen for PowerMenuScreen {
    fn scrim_tap(&self) -> Option<MenuMsg> {
        Some(MenuMsg::Cancel)
    }
}

fn item<'a>(ui: &'a Ui<MenuMsg>, action: PowerAction) -> Element<'a, MenuMsg> {
    let (glyph_color, text_color) = match action.confirmation() {
        Some(_) => (colors::DESTRUCTIVE, colors::DESTRUCTIVE),
        None => (colors::CARD_ICON, ColorRole::Text),
    };
    ui.row([ui.icon(icon(action.glyph())).foreground(glyph_color), ui.text(ui.theme().fonts.body, action.label()).foreground(text_color)])
        .style(&ITEM)
        .height(ITEM_HEIGHT)
        .padding_x(20)
        .gap(16)
        .align(Align::Center)
        .on_tap(MenuMsg::Pick(action))
}
