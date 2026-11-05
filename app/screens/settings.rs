/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use devkit_ui::widgets::{DropdownMenu, dropdown, segmented, slider, slider_value, switch, table, table_chevron, table_header, table_label, table_row, table_section, table_value, top_bar};
use devkit_ui::{Align, AutoRotation, ColorMode, Component, Cx, Drag, Element, Input, Screen, Ui, id};
use idf::sync::OnceLock;

use crate::fonts::icon;
use crate::resources::{colors, icons};
use crate::settings::{self, SoundPlayback};

static ROTATION: OnceLock<AutoRotation> = OnceLock::new();
static INPUT: OnceLock<Input> = OnceLock::new();

pub fn set_input(input: Input) {
    let _ = INPUT.set(input);
}

pub fn set_rotation(rotation: AutoRotation) {
    let _ = ROTATION.set(rotation);
}

#[derive(Clone, Copy)]
pub enum SettingsMsg {
    Back,
    Open,
    Brightness(u8),
    ColorBits(usize),
    Dark(bool),
    RotationLock,
    Volume(u8),
    Equalizer(bool),
    OpenPlayback,
    Playback(usize),
}

const PLAYBACKS: [(SoundPlayback, &str); 2] = [(SoundPlayback::Tab5, "M5Stack Tab5"), (SoundPlayback::Android, "Android Device")];
const PLAYBACK_LABELS: [&str; 2] = [PLAYBACKS[0].1, PLAYBACKS[1].1];

pub struct SettingsScreen {
    color_bits: usize,
    volume: u8,
    equalizer: bool,
}

impl SettingsScreen {
    pub(crate) fn new() -> Self {
        Self { color_bits: 1, volume: 255, equalizer: true }
    }
}

fn slider_row<'a, M: Copy>(ui: &'a Ui<M>, label: &'a str, text: &'a str, value: u8, on_drag: fn(Drag) -> M) -> Element<'a, M> {
    let theme = ui.theme();
    let header = ui.row([table_label(ui, label), ui.text(theme.fonts.caption, text).foreground(colors::MUTED)]).align(Align::Center);
    table_row(ui, [ui.column([header, slider(ui, value, on_drag)]).padding_each(0, theme.widgets.unit.padding, 0, theme.widgets.unit.gap).grow(1)])
}

impl Component for SettingsScreen {
    type Msg = SettingsMsg;

    fn update(&mut self, msg: SettingsMsg, cx: &mut Cx<Self>) {
        match msg {
            SettingsMsg::Back => cx.nav().pop(),
            SettingsMsg::Open => {}
            SettingsMsg::Brightness(value) => settings::set_brightness((u32::from(value) * 100 / 255) as u8),
            SettingsMsg::ColorBits(index) => self.color_bits = index,
            SettingsMsg::Dark(dark) => {
                if let Some(input) = INPUT.get() {
                    input.set_color_mode(if dark { ColorMode::Dark } else { ColorMode::Light });
                }
            }
            SettingsMsg::RotationLock => {
                if let Some(rotation) = ROTATION.get() {
                    rotation.set_locked(!rotation.locked());
                }
            }
            SettingsMsg::Volume(value) => self.volume = value,
            SettingsMsg::Equalizer(on) => self.equalizer = on,
            SettingsMsg::OpenPlayback => {
                if let Some(anchor) = cx.tap_rect() {
                    cx.popup(DropdownMenu::new(anchor, &PLAYBACK_LABELS, playback_index(), cx.handle(), SettingsMsg::Playback));
                }
            }
            SettingsMsg::Playback(index) => settings::set_sound_playback(PLAYBACKS[index].0),
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<SettingsMsg>) -> Element<'a, SettingsMsg> {
        let c = ui.colors();
        let percent = |value: u8| u32::from(value) * 100 / 255;
        let dark = ui.color_mode() == ColorMode::Dark;
        let brightness = settings::brightness();
        let brightness_value = (u32::from(brightness) * 255 / 100) as u8;
        ui.column([
            top_bar(ui, Some(SettingsMsg::Back), "Settings", ui.empty()).scroll_border(id!("content")),
            table(
                ui,
                [
                    table_section(ui, [table_row(ui, [ui.icon(icon(icons::WIFI)), table_label(ui, "Wi-Fi"), table_value(ui, "example-ap"), table_chevron(ui)]).on_tap(SettingsMsg::Open).id(id!("wifi"))]),
                    table_header(ui, "DISPLAY"),
                    table_section(
                        ui,
                        [
                            slider_row(ui, "Brightness", ui.format(format_args!("{brightness}%")), brightness_value, |d| SettingsMsg::Brightness(slider_value(d))).id(id!("brightness")),
                            table_row(ui, [table_label(ui, "Color Mode"), segmented(ui, &[("16-bit", SettingsMsg::ColorBits(0)), ("24-bit", SettingsMsg::ColorBits(1))], self.color_bits)]),
                            table_row(ui, [table_label(ui, "Dark Mode"), switch(ui, dark, SettingsMsg::Dark(!dark)).id(id!("dark"))]),
                            table_row(ui, [table_label(ui, "Rotation Lock"), switch(ui, ROTATION.get().is_some_and(AutoRotation::locked), SettingsMsg::RotationLock).id(id!("rotation-lock"))]),
                        ],
                    ),
                    table_header(ui, "AUDIO"),
                    table_section(
                        ui,
                        [
                            slider_row(ui, "Volume", ui.format(format_args!("{}", percent(self.volume))), self.volume, |d| SettingsMsg::Volume(slider_value(d))).id(id!("volume")),
                            table_row(ui, [table_label(ui, "Equalizer"), switch(ui, self.equalizer, SettingsMsg::Equalizer(!self.equalizer)).id(id!("equalizer"))]),
                            table_row(ui, [table_label(ui, "Sound Playback"), dropdown(ui, PLAYBACK_LABELS[playback_index()], SettingsMsg::OpenPlayback).id(id!("playback"))]),
                        ],
                    ),
                    table_header(ui, "ABOUT"),
                    table_section(
                        ui,
                        [
                            table_row(ui, [table_label(ui, "Version"), table_value(ui, idf::system::app_description().version())]),
                            table_row(ui, [table_label(ui, "Acknowledgements"), table_chevron(ui)]).on_tap(SettingsMsg::Open).id(id!("acknowledgements")),
                        ],
                    ),
                ],
            )
            .grow(1)
            .id(id!("content")),
        ])
        .padding_each(0, ui.insets().top, 0, 0)
        .background(c.background)
    }
}

impl Screen for SettingsScreen {
    fn will_disappear(&mut self, _: &mut Cx<Self>) {
        settings::commit();
    }
}

fn playback_index() -> usize {
    let current = settings::sound_playback();
    PLAYBACKS.iter().position(|&(playback, _)| playback == current).unwrap_or(0)
}
