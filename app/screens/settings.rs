/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use core::ffi::CStr;

use agent_link::AudioVolume;
use agent_link::protocol::VOLUME_SCALE;
use bsp_rs::display;
use devkit_net::wifi_manager::State as WifiState;
use devkit_ui::widgets::{DropdownMenu, dropdown, segmented, slider, slider_value, switch, table, table_chevron, table_header, table_label, table_row, table_section, table_value, top_bar};
use devkit_ui::{Align, AutoRotation, ColorMode, Component, Cx, Drag, Element, Input, RenderMode, Screen, Strips, Ui, id};
use idf::heap::Caps;
use idf::log::error;
use idf::sync::OnceLock;

use crate::fonts::icon;
use crate::resources::{colors, icons};
use crate::settings::{self, AgentMode, AudioPlayback, ColorDepth, MirrorOrientation};
use crate::wifi;
use crate::STRIP_LANES;
use crate::sram::{Sram, UI_BYTES};

use super::wifi::WifiScreen;

const TAG: &CStr = c"settings";

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
    Wifi,
    Brightness(u8),
    ColorDepth(usize),
    ColorDepthApplied(ColorDepth),
    StripsRestored,
    Dark(bool),
    RotationLock,
    Volume(u8),
    Equalizer(bool),
    OpenMirrorPlayback,
    MirrorPlayback(usize),
    OpenNowPlayingPlayback,
    NowPlayingPlayback(usize),
    AgentMode(usize),
    OpenOrientation,
    Orientation(usize),
}

const PLAYBACKS: [(AudioPlayback, &str); 2] = [(AudioPlayback::Tab5, "M5Stack Tab5"), (AudioPlayback::Android, "Android Device")];
const PLAYBACK_LABELS: [&str; 2] = [PLAYBACKS[0].1, PLAYBACKS[1].1];
const COLOR_DEPTHS: [ColorDepth; 2] = [ColorDepth::Bits16, ColorDepth::Bits24];
const AGENT_MODES: [AgentMode; 2] = [AgentMode::Normal, AgentMode::Limited];
const ORIENTATIONS: [(MirrorOrientation, &str); 3] = [(MirrorOrientation::Device, "Same as Device"), (MirrorOrientation::Fit, "Fit to Tab5"), (MirrorOrientation::Rotate, "Rotate Device")];
const ORIENTATION_LABELS: [&str; 3] = [ORIENTATIONS[0].1, ORIENTATIONS[1].1, ORIENTATIONS[2].1];
const AGENT_MODE_NOTE: &str = "Normal runs tab5adb-agent on the phone for screen mirroring, the live preview, and app icons. Limited uses ADB only. Applies on the next connection.";

pub struct SettingsScreen {
    switching: bool,
    equalizer: bool,
}

impl SettingsScreen {
    pub(crate) fn new() -> Self {
        Self { switching: false, equalizer: true }
    }

    fn switch_color_depth(&mut self, depth: ColorDepth, cx: &mut Cx<Self>) {
        if self.switching || depth == settings::color_depth() {
            return;
        }
        let strips = match Strips::allocate(UI_BYTES, Caps::SPIRAM) {
            Ok(strips) => strips,
            Err(err) => {
                error!(TAG, "color depth: strips: {err}");
                return;
            }
        };
        let mut display = display::Config::default();
        display.pixel_format = Some(depth.pixel_format());
        cx.set_render_mode(RenderMode { strips, lanes: STRIP_LANES, display }, SettingsMsg::ColorDepthApplied(depth));
        self.switching = true;
    }

    // Reconfiguring the display needs strips other than the ones in use, so it
    // draws in PSRAM for one frame and then takes the shared SRAM back.
    fn restore_strips(&mut self, depth: ColorDepth, cx: &mut Cx<Self>) {
        let Some(sram) = cx.take_strips().and_then(Strips::into_inner::<Sram>) else {
            error!(TAG, "color depth: display refused the change");
            self.switching = false;
            return;
        };
        settings::set_color_depth(depth);
        cx.set_render_mode(RenderMode { strips: Strips::new(sram), lanes: STRIP_LANES, display: Default::default() }, SettingsMsg::StripsRestored);
    }
}

fn slider_row<'a, M: Copy>(ui: &'a Ui<M>, label: &'a str, text: &'a str, value: u8, on_drag: fn(Drag) -> M) -> Element<'a, M> {
    let theme = ui.theme();
    let header = ui.row([table_label(ui, label), ui.text(theme.fonts.caption, text).foreground(colors::MUTED)]).align(Align::Center);
    table_row(ui, [ui.column([header, slider(ui, value, on_drag)]).padding_each(0, theme.widgets.unit.padding, 0, theme.widgets.unit.gap).grow(1)])
}

fn agent_mode_row<'a>(ui: &'a Ui<SettingsMsg>) -> Element<'a, SettingsMsg> {
    let theme = ui.theme();
    let modes = segmented(ui, &[("Normal", SettingsMsg::AgentMode(0)), ("Limited", SettingsMsg::AgentMode(1))], agent_mode_index());
    let header = ui.row([table_label(ui, "Agent Mode"), modes]).align(Align::Center);
    let note = ui.text(theme.fonts.caption, AGENT_MODE_NOTE).foreground(colors::MUTED).wrap();
    table_row(ui, [ui.column([header, note]).gap(theme.widgets.unit.gap).padding_each(0, theme.widgets.unit.padding, 0, theme.widgets.unit.padding).grow(1)])
}

impl Component for SettingsScreen {
    type Msg = SettingsMsg;

    fn update(&mut self, msg: SettingsMsg, cx: &mut Cx<Self>) {
        match msg {
            SettingsMsg::Back => cx.nav().pop(),
            SettingsMsg::Open => {}
            SettingsMsg::Wifi => cx.nav().push(WifiScreen::new()),
            SettingsMsg::Brightness(value) => settings::set_brightness((u32::from(value) * 100 / 255) as u8),
            SettingsMsg::ColorDepth(index) => self.switch_color_depth(COLOR_DEPTHS[index], cx),
            SettingsMsg::ColorDepthApplied(depth) => self.restore_strips(depth, cx),
            SettingsMsg::StripsRestored => {
                drop(cx.take_strips());
                self.switching = false;
            }
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
            SettingsMsg::Volume(value) => {
                let scale = u32::from(VOLUME_SCALE);
                settings::set_phone_volume(AudioVolume { level: ((u32::from(value) * scale + 127) / 255) as u16, muted: false });
            }
            SettingsMsg::Equalizer(on) => self.equalizer = on,
            SettingsMsg::OpenMirrorPlayback => {
                if let Some(anchor) = cx.tap_rect() {
                    cx.popup(DropdownMenu::new(anchor, &PLAYBACK_LABELS, playback_index(settings::mirror_playback()), cx.handle(), SettingsMsg::MirrorPlayback));
                }
            }
            SettingsMsg::MirrorPlayback(index) => settings::set_mirror_playback(PLAYBACKS[index].0),
            SettingsMsg::OpenNowPlayingPlayback => {
                if let Some(anchor) = cx.tap_rect() {
                    cx.popup(DropdownMenu::new(anchor, &PLAYBACK_LABELS, playback_index(settings::now_playing_playback()), cx.handle(), SettingsMsg::NowPlayingPlayback));
                }
            }
            SettingsMsg::NowPlayingPlayback(index) => settings::set_now_playing_playback(PLAYBACKS[index].0),
            SettingsMsg::AgentMode(index) => settings::set_agent_mode(AGENT_MODES[index]),
            SettingsMsg::OpenOrientation => {
                if let Some(anchor) = cx.tap_rect() {
                    cx.popup(DropdownMenu::new(anchor, &ORIENTATION_LABELS, orientation_index(), cx.handle(), SettingsMsg::Orientation));
                }
            }
            SettingsMsg::Orientation(index) => settings::set_mirror_orientation(ORIENTATIONS[index].0),
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<SettingsMsg>) -> Element<'a, SettingsMsg> {
        let c = ui.colors();
        let volume = u32::from(settings::phone_volume().level);
        let scale = u32::from(VOLUME_SCALE);
        let volume_value = ((volume * 255 + scale / 2) / scale) as u8;
        let dark = ui.color_mode() == ColorMode::Dark;
        let brightness = settings::brightness();
        let brightness_value = (u32::from(brightness) * 255 / 100) as u8;
        ui.column([
            top_bar(ui, Some(SettingsMsg::Back), "Settings", ui.empty()).scroll_border(id!("content")),
            table(
                ui,
                [
                    table_section(ui, [wifi_row(ui)]),
                    table_header(ui, "DISPLAY"),
                    table_section(
                        ui,
                        [
                            slider_row(ui, "Brightness", ui.format(format_args!("{brightness}%")), brightness_value, |d| SettingsMsg::Brightness(slider_value(d))).id(id!("brightness")),
                            table_row(ui, [table_label(ui, "Color Mode"), segmented(ui, &[("16-bit", SettingsMsg::ColorDepth(0)), ("24-bit", SettingsMsg::ColorDepth(1))], color_depth_index())]),
                            table_row(ui, [table_label(ui, "Dark Mode"), switch(ui, dark, SettingsMsg::Dark(!dark)).id(id!("dark"))]),
                            table_row(ui, [table_label(ui, "Rotation Lock"), switch(ui, ROTATION.get().is_some_and(AutoRotation::locked), SettingsMsg::RotationLock).id(id!("rotation-lock"))]),
                        ],
                    ),
                    table_header(ui, "AUDIO"),
                    table_section(
                        ui,
                        [
                            slider_row(ui, "Volume", ui.format(format_args!("{}", volume * 100 / scale)), volume_value, |d| SettingsMsg::Volume(slider_value(d))).id(id!("volume")),
                            table_row(ui, [table_label(ui, "Equalizer"), switch(ui, self.equalizer, SettingsMsg::Equalizer(!self.equalizer)).id(id!("equalizer"))]),
                        ],
                    ),
                    table_header(ui, "ANDROID DEVICE"),
                    table_section(
                        ui,
                        [agent_mode_row(ui)],
                    ),
                    table_header(ui, "MIRRORING"),
                    table_section(
                        ui,
                        [
                            table_row(ui, [table_label(ui, "Orientation"), dropdown(ui, ORIENTATION_LABELS[orientation_index()], SettingsMsg::OpenOrientation).id(id!("orientation"))]),
                            playback_row(ui, settings::mirror_playback(), SettingsMsg::OpenMirrorPlayback).id(id!("mirror-playback")),
                        ],
                    ),
                    table_header(ui, "NOW PLAYING"),
                    table_section(ui, [playback_row(ui, settings::now_playing_playback(), SettingsMsg::OpenNowPlayingPlayback).id(id!("now-playing-playback"))]),
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

fn color_depth_index() -> usize {
    let current = settings::color_depth();
    COLOR_DEPTHS.iter().position(|&depth| depth == current).unwrap_or(0)
}

fn agent_mode_index() -> usize {
    let current = settings::agent_mode();
    AGENT_MODES.iter().position(|&mode| mode == current).unwrap_or(0)
}

fn orientation_index() -> usize {
    let current = settings::mirror_orientation();
    ORIENTATIONS.iter().position(|&(orientation, _)| orientation == current).unwrap_or(0)
}

fn playback_index(current: AudioPlayback) -> usize {
    PLAYBACKS.iter().position(|&(playback, _)| playback == current).unwrap_or(0)
}

fn wifi_row<'a>(ui: &'a Ui<SettingsMsg>) -> Element<'a, SettingsMsg> {
    let value = match wifi::state() {
        WifiState::Connected { ssid, .. } => ui.store(&wifi::ssid(&ssid)),
        WifiState::Disabled => "Off",
        _ => "Not connected",
    };
    table_row(ui, [ui.icon(icon(icons::WIFI)), table_label(ui, "Wi-Fi"), table_value(ui, value), table_chevron(ui)])
        .on_tap(SettingsMsg::Wifi)
        .id(id!("wifi"))
        .task(|this| wifi::follow(this, |_: &mut SettingsScreen| {}))
}

fn playback_row<'a>(ui: &'a Ui<SettingsMsg>, current: AudioPlayback, open: SettingsMsg) -> Element<'a, SettingsMsg> {
    table_row(ui, [table_label(ui, "Audio Playback"), dropdown(ui, PLAYBACK_LABELS[playback_index(current)], open)])
}
