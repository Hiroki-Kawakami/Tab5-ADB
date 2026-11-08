/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::String;
use alloc::vec::Vec;
use core::ffi::CStr;
use core::future::{Future, pending, poll_fn};
use core::pin::pin;
use core::task::Poll;

use adb::link::{Connection, State as LinkState};
use agent_link::Link;
use devkit_gfx::Color;
use devkit_ui::widgets::{divider, vertical_divider};
use devkit_ui::{Align, ColorRole, Component, Cx, Drag, Element, Icon, Justify, Overlay, Prop, Root, Screen, State, StateStyle, Task, This, Transition, Ui, id};
use idf::log::error;

use crate::adb_link::Route;
use crate::agent::Agent;
use crate::device_info::{self, DeviceInfo};
use crate::fonts::{icon, small_icon};
use crate::preview::{self, Preview};
use crate::resources::{colors, icons};

use super::device_info::DeviceInfoScreen;
use super::message::MessageScreen;
use super::mirror::{MirrorScreen, keycode};
use super::settings::SettingsScreen;
use super::terminal::TerminalScreen;

const TAG: &CStr = c"dashboard";

const GAP: i16 = 20;
const TILE_GAP: i16 = 12;
const RADIUS: u8 = 20;
const PREVIEW_WIDTH_PORTRAIT: i16 = 360;
const PREVIEW_WIDTH_LANDSCAPE: i16 = 288;
const PREVIEW_HEIGHT_PORTRAIT: i16 = 852;
const PREVIEW_HEIGHT_LANDSCAPE: i16 = 640;
const NAV_BAR_SIZE: i16 = 80;
const NAV_BAR_SIZE_LANDSCAPE: i16 = 72;

const PREVIEW_BG: Color = Color::hex(0x111418);

static TILE: StateStyle = StateStyle {
    font: None,
    base: &[Prop::Background(ColorRole::Surface), Prop::Radius(RADIUS)],
    states: &[(State::PRESSED, &[Prop::Background(colors::PRESSED)])],
};

#[derive(Clone, Copy)]
pub enum Msg {
    Info,
    Preview,
    VideoUnavailable,
    Key(u16, bool),
    Shell,
    FileManager,
    Apps,
    Logcat,
    Screenshot,
    PowerMenu,
    Settings,
    Disconnect,
    Previous,
    Play,
    Next,
}

const FEATURES: [(char, &str, Msg); 8] = [
    (icons::TERMINAL, "Shell", Msg::Shell),
    (icons::FOLDER, "File Manager", Msg::FileManager),
    (icons::APPS, "Apps", Msg::Apps),
    (icons::LIST, "Logcat", Msg::Logcat),
    (icons::CAMERA, "Screenshot", Msg::Screenshot),
    (icons::POWER, "Power Menu", Msg::PowerMenu),
    (icons::SETTINGS, "Settings", Msg::Settings),
    (icons::DISCONNECT, "Disconnect", Msg::Disconnect),
];

pub struct AdbDashboardScreen {
    conn: Connection,
    route: Route,
    model: Option<String>,
    info: Option<DeviceInfo>,
    watch: Option<Task>,
    agent: Option<Agent>,
    video_unavailable: bool,
    /// Keys pressed, so that a held finger sends one press.
    keys: Vec<u16>,
}

impl AdbDashboardScreen {
    /// Without `agent`, only what plain adb can do.
    pub fn new(conn: Connection, route: Route, agent: Option<Agent>) -> Self {
        let model = device_info::model(&conn.banner());
        Self { conn, route, model, info: None, watch: None, agent, video_unavailable: false, keys: Vec::new() }
    }

    fn key(&mut self, code: u16, down: bool, cx: &mut Cx<Self>) {
        if down == self.keys.contains(&code) {
            return;
        }
        if down {
            self.keys.push(code);
        } else {
            self.keys.retain(|&k| k != code);
        }
        if let Some(agent) = &self.agent {
            if let Err(err) = agent.link().key(code, down) {
                error!(TAG, "key {code}: {err}");
            }
            return;
        }
        if down {
            return;
        }
        let conn = self.conn.clone();
        cx.spawn(move |_| async move {
            if let Err(err) = adb::services::exec(&conn, &alloc::format!("input keyevent {code}")).await {
                error!(TAG, "input keyevent {code}: {err}");
            }
        });
    }

    fn header<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let theme = ui.theme();
        let info = self.info.as_ref();
        let name = info.and_then(|i| i.name.as_deref()).or(self.model.as_deref()).unwrap_or("Android device");
        let route = match self.route {
            Route::Usb => "USB",
            Route::Tcp(_) => "Wireless",
        };
        let caption = match info {
            Some(i) => {
                let version = if i.version.is_empty() { String::new() } else { alloc::format!("Android {} \u{2022} ", i.version) };
                let storage = i.storage_kb.map(|kb| alloc::format!("{} GB \u{2022} ", (kb * 1024 + 500_000_000) / 1_000_000_000)).unwrap_or_default();
                ui.format(format_args!("{version}{storage}{route}"))
            }
            None => route,
        };
        let battery = match info.and_then(|i| i.battery) {
            Some(b) => {
                let (glyph, color) = if b.charging { (icons::BATTERY_CHARGING, ui.colors().primary) } else { (icons::BATTERY, ui.colors().text) };
                ui.row([ui.icon(small_icon(glyph)).foreground(color), ui.text(theme.fonts.caption, ui.format(format_args!("{}%", b.level))).foreground(color)]).gap(4).align(Align::Center)
            }
            None => ui.empty(),
        };
        let wifi = if info.is_some_and(|i| i.wifi) { ui.icon(small_icon(icons::WIFI)).foreground(colors::CARD_ICON) } else { ui.empty() };
        let status = ui.row([battery, wifi, ui.icon(theme.icons.chevron).foreground(colors::MUTED)]).gap(16).align(Align::Center);
        ui.row([ui.column([ui.text(theme.fonts.title, name), ui.text(theme.fonts.caption, caption).foreground(colors::MUTED)]).gap(4).grow(1), status])
            .style(&TILE)
            .height(104)
            .padding_x(24)
            .align(Align::Center)
            .on_tap(Msg::Info)
    }

    fn preview<'a>(&self, ui: &'a Ui<Msg>, width: i16, height: i16) -> Element<'a, Msg> {
        let props = preview::Props { width: width as u16, height: height as u16, screencap: self.agent.is_none() || self.video_unavailable };
        ui.child_mapped::<Preview>(props, |_| Msg::VideoUnavailable).radius(RADIUS).background(PREVIEW_BG).clip().on_tap(Msg::Preview).id(id!("preview"))
    }
}

async fn load_info(this: This<AdbDashboardScreen>, conn: Connection) {
    match adb::services::exec(&conn, device_info::COMMAND).await {
        Ok(out) => {
            let info = device_info::parse(&String::from_utf8_lossy(&out));
            this.with(|s, cx| {
                s.info = Some(info);
                cx.rebuild();
            });
        }
        Err(err) => error!(TAG, "device info: {err}"),
    }
}

async fn watch(this: This<AdbDashboardScreen>, conn: Connection, link: Option<Link>) {
    let mut adb = pin!(async {
        while !matches!(conn.state(), LinkState::Closed(_)) {
            conn.state_changed().await;
        }
    });
    let mut agent = pin!(async {
        match &link {
            Some(link) => drop(link.closed().await),
            None => pending().await,
        }
    });
    let message = poll_fn(|cx| {
        if adb.as_mut().poll(cx).is_ready() {
            Poll::Ready("The device was disconnected.")
        } else if agent.as_mut().poll(cx).is_ready() {
            Poll::Ready("The connection to the agent was lost.")
        } else {
            Poll::Pending
        }
    })
    .await;
    this.with(|_, cx| {
        cx.nav_of::<Root>().root();
        cx.nav_of::<Overlay>().push(MessageScreen::new("Disconnected", message));
    });
}

impl Component for AdbDashboardScreen {
    type Msg = Msg;

    fn update(&mut self, msg: Msg, cx: &mut Cx<Self>) {
        match msg {
            Msg::Disconnect => {
                if let Some(task) = self.watch.take() {
                    task.abort();
                }
                self.agent = None;
                cx.nav_of::<Root>().root();
            }
            Msg::Info => cx.nav_of::<Root>().push(DeviceInfoScreen::new(self.conn.clone())),
            Msg::Shell => cx.nav_of::<Root>().push(TerminalScreen::new(self.conn.clone())),
            Msg::Settings => cx.nav_of::<Root>().transition(Transition::SlideUp).push(SettingsScreen::new()),
            Msg::Preview => match &self.agent {
                Some(_) if self.video_unavailable => {
                    cx.nav_of::<Overlay>().push(MessageScreen::new("Mirroring unavailable", "The agent cannot capture this device's screen, so mirroring is disabled."));
                }
                Some(agent) => {
                    if let Some(screen) = MirrorScreen::new(agent.link().clone()) {
                        cx.nav_of::<Root>().transition(Transition::Zoom).push(screen);
                    }
                }
                None => {
                    cx.nav_of::<Overlay>().push(MessageScreen::new("Mirroring unavailable", "The agent could not be started on this device, so mirroring is disabled."));
                }
            },
            Msg::VideoUnavailable => {
                self.video_unavailable = true;
                cx.rebuild();
            }
            Msg::Key(code, down) => self.key(code, down, cx),
            _ => {}
        }
    }

    fn view<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let (width, height) = ui.screen_size();
        let body = if width > height {
            let nav = ui.column([nav_bar(ui, true)]).justify(Justify::End);
            let preview = self.preview(ui, PREVIEW_WIDTH_LANDSCAPE, PREVIEW_HEIGHT_LANDSCAPE).width(PREVIEW_WIDTH_LANDSCAPE);
            let rows: Vec<_> = FEATURES.chunks(4).map(|row| ui.row(row.iter().map(|&(glyph, label, msg)| feature_tile(ui, glyph, label, msg)).collect::<Vec<_>>()).gap(TILE_GAP).grow(1)).collect();
            let main = ui.column([self.header(ui), ui.column(rows).gap(TILE_GAP).grow(1), media(ui)]).gap(GAP).grow(1);
            ui.row([ui.row([nav, preview]).gap(TILE_GAP), main])
        } else {
            let tiles: Vec<_> = FEATURES.iter().map(|&(glyph, label, msg)| feature_row(ui, glyph, label, msg)).collect();
            let left = ui.column([self.preview(ui, PREVIEW_WIDTH_PORTRAIT, PREVIEW_HEIGHT_PORTRAIT).grow(1), nav_bar(ui, false)]).gap(TILE_GAP).width(PREVIEW_WIDTH_PORTRAIT);
            let middle = ui.row([left, ui.column(tiles).gap(TILE_GAP).grow(1)]).gap(GAP).grow(1);
            ui.column([self.header(ui), middle, media(ui)])
        };
        body.gap(GAP)
            .padding_each(24, ui.insets().top + 4, 24, 24)
            .background(ui.colors().background)
    }
}

impl Screen for AdbDashboardScreen {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        let conn = self.conn.clone();
        cx.spawn(|this| load_info(this, conn));
        let (conn, link) = (self.conn.clone(), self.agent.as_ref().map(|a| a.link().clone()));
        self.watch = Some(cx.spawn(|this| watch(this, conn, link)));
    }

    fn will_disappear(&mut self, _: &mut Cx<Self>) {
        self.keys.clear();
        if let Some(agent) = &self.agent
            && let Err(err) = agent.link().input_clear()
        {
            error!(TAG, "input clear: {err}");
        }
    }
}

fn key_msg<const CODE: u16>(drag: Drag) -> Msg {
    Msg::Key(CODE, !drag.ended)
}

fn nav_bar<'a>(ui: &'a Ui<Msg>, vertical: bool) -> Element<'a, Msg> {
    let mut keys: [(char, fn(Drag) -> Msg); 4] = [
        (icons::NAV_BACK, key_msg::<{ keycode::BACK }>),
        (icons::NAV_HOME, key_msg::<{ keycode::HOME }>),
        (icons::NAV_RECENTS, key_msg::<{ keycode::APP_SWITCH }>),
        (icons::POWER, key_msg::<{ keycode::POWER }>),
    ];
    if vertical {
        keys.reverse();
    }
    let mut items = Vec::new();
    for (i, &(glyph, on_drag)) in keys.iter().enumerate() {
        if i > 0 && (glyph == icons::POWER || keys[i - 1].0 == icons::POWER) {
            items.push(if vertical { divider(ui).padding_x(ui.theme().widgets.unit.gap) } else { vertical_divider(ui) });
        }
        items.push(nav_key(ui, icon(glyph), on_drag).grow(1));
    }
    let bar = if vertical { ui.column(items).width(NAV_BAR_SIZE_LANDSCAPE).padding_y(8) } else { ui.row(items).height(NAV_BAR_SIZE) }.align(Align::Center);
    bar.radius(RADIUS).background(ui.colors().surface)
}

fn nav_key<'a>(ui: &'a Ui<Msg>, icon: Icon, on_drag: fn(Drag) -> Msg) -> Element<'a, Msg> {
    ui.row([key(ui, icon).inherit_state(State::PRESSED)]).align(Align::Center).justify(Justify::Center).state_source().on_drag(on_drag)
}

fn key<'a>(ui: &'a Ui<Msg>, icon: Icon) -> Element<'a, Msg> {
    ui.row([ui.icon(icon).foreground(colors::CARD_ICON)])
        .size(64, 64)
        .radius(14)
        .align(Align::Center)
        .justify(Justify::Center)
        .when(State::PRESSED, &[Prop::Background(colors::PRESSED)])
}

fn feature_color(msg: Msg) -> ColorRole {
    match msg {
        Msg::Disconnect => colors::DESTRUCTIVE,
        _ => ColorRole::Text,
    }
}

fn feature_icon_color(msg: Msg) -> ColorRole {
    match msg {
        Msg::Disconnect => colors::DESTRUCTIVE,
        _ => colors::CARD_ICON,
    }
}

fn feature_row<'a>(ui: &'a Ui<Msg>, glyph: char, label: &'a str, msg: Msg) -> Element<'a, Msg> {
    ui.row([ui.icon(icon(glyph)).foreground(feature_icon_color(msg)), ui.text(ui.theme().fonts.body, label).foreground(feature_color(msg))])
        .style(&TILE)
        .grow(1)
        .padding_x(24)
        .gap(16)
        .align(Align::Center)
        .on_tap(msg)
}

fn feature_tile<'a>(ui: &'a Ui<Msg>, glyph: char, label: &'a str, msg: Msg) -> Element<'a, Msg> {
    ui.column([ui.icon(icon(glyph)).foreground(feature_icon_color(msg)), ui.text(ui.theme().fonts.body, label).foreground(feature_color(msg))])
        .style(&TILE)
        .grow(1)
        .gap(12)
        .align(Align::Center)
        .justify(Justify::Center)
        .on_tap(msg)
}

fn media<'a>(ui: &'a Ui<Msg>) -> Element<'a, Msg> {
    let theme = ui.theme();
    let art = ui.row([ui.icon(icon(icons::MUSIC)).foreground(colors::MUTED)]).size(72, 72).radius(12).align(Align::Center).justify(Justify::Center).background(ui.colors().background);
    let title = ui.column([ui.text(theme.fonts.body, "Track Title"), ui.text(theme.fonts.caption, "Artist \u{2022} App").foreground(colors::MUTED)]).gap(4).grow(1);
    let controls = ui.row([key(ui, icon(icons::PREVIOUS)).radius(32).on_tap(Msg::Previous), key(ui, icon(icons::PLAY)).radius(32).on_tap(Msg::Play), key(ui, icon(icons::NEXT)).radius(32).on_tap(Msg::Next)]).gap(14).align(Align::Center);
    ui.row([art, title, controls]).height(112).padding_x(20).gap(16).align(Align::Center).radius(RADIUS).background(ui.colors().surface)
}
