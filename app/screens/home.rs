/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::{String, ToString};
use alloc::vec;
use alloc::vec::Vec;

use devkit_gfx::{Color, FontChain};
use devkit_net::wifi_manager::State as WifiState;
use devkit_ui::widgets::text_field;
use devkit_ui::{Align, ColorRole, Component, Cx, Element, Handle, Icon, Justify, Overlay, Prop, Root, Screen, Sides, State, StateStyle, TextBuffer, TextInput, Transition, Ui};
use idf::sync::Mutex;

use crate::fonts::{HERO, icon, small_icon};
use crate::resources::{colors, icons};
use crate::{adb_link, wifi};

use super::connecting::ConnectingScreen;
use super::file_manager::FileManagerScreen;
use super::message::MessageScreen;
use super::pair_device::PairDeviceScreen;
use super::settings::SettingsScreen;

const GAP: i16 = 20;
const CARD_PADDING: i16 = 24;
const CARD_RADIUS: u8 = 24;
const SIDEBAR_WIDTH: i16 = 400;

const HERO_BG: Color = Color::hex(0x111418);
const HERO_TEXT: Color = Color::hex(0xffffff);
const ACCENT: Color = Color::hex(0x3ddc84);
const VERSION: Color = Color::hex(0x90a4ae);
const NAV_ICON: Color = Color::hex(0xb0bec5);

static NAV: StateStyle = StateStyle {
    font: None,
    base: &[Prop::Background(ColorRole::Fixed(Color::hex(0x22272e))), Prop::Foreground(ColorRole::Fixed(HERO_TEXT)), Prop::Radius(14)],
    states: &[(State::PRESSED, &[Prop::Background(ColorRole::Fixed(Color::hex(0x2e353e)))])],
};

static HOME: Mutex<Option<Handle<Msg>>> = Mutex::new(None);
static REQUESTED: Mutex<Option<String>> = Mutex::new(None);

#[derive(Clone, Copy)]
pub enum Msg {
    Settings,
    Files,
    GitHub,
    UsbConnect,
    Pair,
    TcpConnect,
    Address(TextInput),
    Recent,
    Requested,
}

/// `adb tcp <host:port>`: connects from the home screen as its Connect
/// button does.
pub fn adb_command(args: &[&str]) -> Result<Option<String>, String> {
    let ["tcp", addr] = args else { return Err("usage: adb tcp <host:port>".into()) };
    let home = HOME.lock().clone().ok_or("no home screen")?;
    *REQUESTED.lock() = Some(addr.to_string());
    if !home.post(Msg::Requested) {
        return Err("UI queue full".into());
    }
    Ok(None)
}

pub struct HomeScreen {
    address: TextBuffer,
    recent: Vec<String>,
}

impl HomeScreen {
    pub fn new() -> Self {
        Self {
            address: TextBuffer::new(),
            recent: vec![String::from("192.168.x.x:36645"), String::from("192.168.x.x:5555")],
        }
    }

    fn hero<'a>(&'a self, ui: &'a Ui<Msg>, landscape: bool) -> Element<'a, Msg> {
        let caption = ui.theme().fonts.caption;
        let title = ui.column([
            ui.row([ui.text(&HERO, "Tab5").foreground(HERO_TEXT), ui.text(&HERO, "ADB").foreground(ACCENT)]).gap(18),
            ui.text(caption, idf::system::app_description().version()).foreground(VERSION),
        ])
        .gap(4);
        let navs = [(icons::SETTINGS, "Settings", Msg::Settings), (icons::FOLDER, "Files", Msg::Files), (icons::GITHUB, "GitHub", Msg::GitHub)];
        let card = if landscape {
            let buttons: Vec<_> = navs.iter().map(|&(glyph, label, msg)| nav_row(ui, icon(glyph), label, msg)).collect();
            ui.column([title, ui.column([]).grow(1), ui.column(buttons).gap(12)]).width(SIDEBAR_WIDTH)
        } else {
            let buttons: Vec<_> = navs.iter().map(|&(glyph, label, msg)| nav_tile(ui, icon(glyph), label, msg)).collect();
            ui.column([title, ui.row(buttons).gap(14)]).gap(32)
        };
        card.padding(28).radius(CARD_RADIUS).background(HERO_BG)
    }

    fn usb<'a>(&'a self, ui: &'a Ui<Msg>, landscape: bool) -> Element<'a, Msg> {
        let header = card_header(ui, icons::USB, "USB");
        let connect = action(ui, ui.theme().fonts.title, "Connect", ui.theme().widgets.button.primary, Msg::UsbConnect);
        if landscape {
            card(ui, ui.row([header.padding_x(8), connect.height(80).grow(1)]).gap(40).align(Align::Center))
        } else {
            card(ui, ui.column([header, connect.height(96)]).gap(16))
        }
    }

    fn wireless<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let theme = ui.theme();
        let caption = theme.fonts.caption;
        let status = match wifi::state() {
            WifiState::Connected { .. } => ui
                .row([ui.column([]).size(8, 8).radius(4).background(colors::ONLINE), ui.text(caption, "Wi-Fi Connected").foreground(colors::ONLINE)])
                .gap(10)
                .align(Align::Center),
            WifiState::Disabled => ui.text(caption, "Wi-Fi Off").foreground(colors::MUTED),
            _ => ui.text(caption, "Wi-Fi not connected").foreground(colors::MUTED),
        };
        let pair = ui
            .row([ui.icon(small_icon(icons::LINK)), ui.text(caption, "Pair Device")])
            .style(theme.widgets.button.plain_outline)
            .height(56)
            .padding_x(20)
            .gap(10)
            .align(Align::Center)
            .on_tap(Msg::Pair);
        let header = ui.row([ui.column([card_header(ui, icons::WIFI, "Wireless (TCP/IP)"), status.padding_x(4)]).gap(8).grow(1), pair]).align(Align::Start);
        let input = ui
            .row([
                text_field(ui, &self.address, Msg::Address).placeholder("192.168.x.x:5555").padding_y(18).grow(1),
                action(ui, caption, "Connect", theme.widgets.button.primary, Msg::TcpConnect).size(160, 64),
            ])
            .gap(12)
            .align(Align::Center);
        let rows: Vec<_> = self.recent.iter().map(|address| recent_row(ui, address, Msg::Recent)).collect();
        let recent = ui.column([ui.text(caption, "RECENT").foreground(colors::MUTED).padding_each(0, 8, 0, 4), ui.column(rows)]);
        card(ui, ui.column([header, input, recent]).gap(20)).grow(1).task(|this| wifi::follow(this, |_: &mut HomeScreen| {}))
    }
}

impl Component for HomeScreen {
    type Msg = Msg;

    fn update(&mut self, msg: Msg, cx: &mut Cx<Self>) {
        match msg {
            Msg::Settings => cx.nav_of::<Root>().transition(Transition::SlideUp).push(SettingsScreen::new()),
            Msg::Files => cx.nav_of::<Root>().push(FileManagerScreen::new(false)),
            Msg::Pair => cx.nav_of::<Root>().push(PairDeviceScreen::new()),
            Msg::UsbConnect => match adb_link::connect() {
                Ok(conn) => cx.nav_of::<Overlay>().push(ConnectingScreen::usb(conn)),
                Err(err) => cx.nav_of::<Overlay>().push(MessageScreen::new("Connection failed", err)),
            },
            Msg::TcpConnect => {
                let addr = self.address.as_str().trim();
                if !addr.is_empty() {
                    cx.nav_of::<Overlay>().push(ConnectingScreen::tcp(addr.into()));
                }
            }
            Msg::Requested => {
                if let Some(addr) = REQUESTED.lock().take() {
                    self.address.set_text(&addr);
                    cx.rebuild();
                    cx.nav_of::<Overlay>().push(ConnectingScreen::tcp(addr));
                }
            }
            Msg::Address(input) => {
                if self.address.apply(input) {
                    cx.rebuild();
                }
            }
            _ => {}
        }
    }

    fn view<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let (width, height) = ui.screen_size();
        let landscape = width > height;
        let body = if landscape {
            ui.row([self.hero(ui, true), ui.column([self.usb(ui, true), self.wireless(ui)]).gap(GAP).grow(1)])
        } else {
            ui.column([self.hero(ui, false), self.usb(ui, false), self.wireless(ui)])
        };
        body.gap(GAP)
            .padding_each(24, ui.insets().top + 4, 24, 24)
            .background(ui.colors().background)
    }
}

impl Screen for HomeScreen {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        *HOME.lock() = Some(cx.handle());
    }
}

fn card<'a>(ui: &'a Ui<Msg>, content: Element<'a, Msg>) -> Element<'a, Msg> {
    ui.column([content]).padding(CARD_PADDING).radius(CARD_RADIUS).background(ui.colors().surface)
}

fn card_header<'a>(ui: &'a Ui<Msg>, glyph: char, title: &'a str) -> Element<'a, Msg> {
    ui.row([ui.icon(icon(glyph)).foreground(colors::CARD_ICON), ui.text(ui.theme().fonts.title, title).foreground(colors::CARD_TITLE)])
        .gap(14)
        .align(Align::Center)
}

fn action<'a>(ui: &'a Ui<Msg>, font: &'static FontChain, label: &'a str, style: &'static StateStyle, msg: Msg) -> Element<'a, Msg> {
    ui.row([ui.text(font, label)]).style(style).padding_x(0).align(Align::Center).justify(Justify::Center).on_tap(msg)
}

fn nav_tile<'a>(ui: &'a Ui<Msg>, icon: Icon, label: &'a str, msg: Msg) -> Element<'a, Msg> {
    ui.column([ui.icon(icon).foreground(NAV_ICON), ui.text(ui.theme().fonts.body, label)])
        .style(&NAV)
        .height(120)
        .grow(1)
        .gap(10)
        .align(Align::Center)
        .justify(Justify::Center)
        .on_tap(msg)
}

fn nav_row<'a>(ui: &'a Ui<Msg>, icon: Icon, label: &'a str, msg: Msg) -> Element<'a, Msg> {
    ui.row([ui.icon(icon).foreground(NAV_ICON), ui.text(ui.theme().fonts.body, label)])
        .style(&NAV)
        .height(80)
        .padding_x(28)
        .gap(20)
        .align(Align::Center)
        .on_tap(msg)
}

fn recent_row<'a>(ui: &'a Ui<Msg>, address: &'a str, msg: Msg) -> Element<'a, Msg> {
    let theme = ui.theme();
    ui.row([
        ui.icon(small_icon(icons::HISTORY)).foreground(colors::MUTED),
        ui.text(theme.fonts.body, address).grow(1),
        ui.icon(theme.icons.chevron).foreground(ui.colors().divider),
    ])
    .height(64)
    .gap(16)
    .align(Align::Center)
    .border(ui.colors().divider, 1)
    .border_sides(Sides::BOTTOM)
    .when(State::PRESSED, &[Prop::Background(colors::PRESSED)])
    .on_tap(msg)
}
