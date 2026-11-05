/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::vec::Vec;

use devkit_gfx::Color;
use devkit_ui::widgets::{divider, vertical_divider};
use devkit_ui::{Align, ColorRole, Component, Cx, Element, Icon, Justify, Prop, Screen, State, StateStyle, Ui};

use crate::fonts::{icon, small_icon};
use crate::resources::{colors, icons};

const GAP: i16 = 20;
const TILE_GAP: i16 = 12;
const RADIUS: u8 = 20;
const PREVIEW_WIDTH_PORTRAIT: i16 = 360;
const PREVIEW_WIDTH_LANDSCAPE: i16 = 288;
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
    Back,
    Home,
    Recents,
    Power,
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

pub struct AdbDashboardScreen;

impl AdbDashboardScreen {
    pub fn new() -> Self {
        Self
    }
}

impl Component for AdbDashboardScreen {
    type Msg = Msg;

    fn update(&mut self, _: Msg, _: &mut Cx<Self>) {}

    fn view<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let (width, height) = ui.screen_size();
        let body = if width > height {
            let nav = ui.column([nav_bar(ui, true)]).justify(Justify::End);
            let preview = preview(ui).width(PREVIEW_WIDTH_LANDSCAPE);
            let rows: Vec<_> = FEATURES.chunks(4).map(|row| ui.row(row.iter().map(|&(glyph, label, msg)| feature_tile(ui, glyph, label, msg)).collect::<Vec<_>>()).gap(TILE_GAP).grow(1)).collect();
            let main = ui.column([header(ui), ui.column(rows).gap(TILE_GAP).grow(1), media(ui)]).gap(GAP).grow(1);
            ui.row([ui.row([nav, preview]).gap(TILE_GAP), main])
        } else {
            let tiles: Vec<_> = FEATURES.iter().map(|&(glyph, label, msg)| feature_row(ui, glyph, label, msg)).collect();
            let left = ui.column([preview(ui).grow(1), nav_bar(ui, false)]).gap(TILE_GAP).width(PREVIEW_WIDTH_PORTRAIT);
            let middle = ui.row([left, ui.column(tiles).gap(TILE_GAP).grow(1)]).gap(GAP).grow(1);
            ui.column([header(ui), middle, media(ui)])
        };
        body.gap(GAP)
            .padding_each(24, ui.insets().top + 4, 24, 24)
            .background(ui.colors().background)
    }
}

impl Screen for AdbDashboardScreen {}

fn header<'a>(ui: &'a Ui<Msg>) -> Element<'a, Msg> {
    let theme = ui.theme();
    let primary = ui.colors().primary;
    let status = ui
        .row([
            ui.row([ui.icon(small_icon(icons::BATTERY_CHARGING)).foreground(primary), ui.text(theme.fonts.caption, "22%").foreground(primary)]).gap(4).align(Align::Center),
            ui.icon(small_icon(icons::WIFI)).foreground(colors::CARD_ICON),
            ui.icon(theme.icons.chevron).foreground(colors::MUTED),
        ])
        .gap(16)
        .align(Align::Center);
    ui.row([
        ui.column([ui.text(theme.fonts.title, "Pixel 10"), ui.text(theme.fonts.caption, "Android 17 \u{2022} 128 GB \u{2022} USB").foreground(colors::MUTED)]).gap(4).grow(1),
        status,
    ])
    .style(&TILE)
    .height(104)
    .padding_x(24)
    .align(Align::Center)
    .on_tap(Msg::Info)
}

fn preview<'a>(ui: &'a Ui<Msg>) -> Element<'a, Msg> {
    ui.column([]).radius(RADIUS).background(PREVIEW_BG).on_tap(Msg::Preview)
}

fn nav_bar<'a>(ui: &'a Ui<Msg>, vertical: bool) -> Element<'a, Msg> {
    let mut keys = [(icons::NAV_BACK, Msg::Back), (icons::NAV_HOME, Msg::Home), (icons::NAV_RECENTS, Msg::Recents), (icons::POWER, Msg::Power)];
    if vertical {
        keys.reverse();
    }
    let mut items = Vec::new();
    for (i, &(glyph, msg)) in keys.iter().enumerate() {
        if i > 0 && (matches!(msg, Msg::Power) || matches!(keys[i - 1].1, Msg::Power)) {
            items.push(if vertical { divider(ui).padding_x(ui.theme().widgets.unit.gap) } else { vertical_divider(ui) });
        }
        items.push(nav_key(ui, icon(glyph), msg).grow(1));
    }
    let bar = if vertical { ui.column(items).width(NAV_BAR_SIZE_LANDSCAPE).padding_y(8) } else { ui.row(items).height(NAV_BAR_SIZE) }.align(Align::Center);
    bar.radius(RADIUS).background(ui.colors().surface)
}

fn nav_key<'a>(ui: &'a Ui<Msg>, icon: Icon, msg: Msg) -> Element<'a, Msg> {
    ui.row([key(ui, icon).inherit_state(State::PRESSED)]).align(Align::Center).justify(Justify::Center).state_source().on_tap(msg)
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
