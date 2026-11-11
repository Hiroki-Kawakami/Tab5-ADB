/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::String;

use devkit_net::wifi_manager::State as WifiState;
use devkit_ui::widgets::{button, button_from, divider, modal, modal_actions, modal_message, modal_title, progress_indicator, text_field, top_bar};
use devkit_ui::{Align, Component, Cx, Element, Elide, Overlay, Root, Screen, Task, TextBuffer, TextInput, This, Ui, id};

use crate::adb_link;
use crate::fonts::icon;
use crate::resources::{TOUCH_PAIR_ADDRESS_KEYBOARD, TOUCH_PAIR_CODE_KEYBOARD, colors, icons};
use crate::wifi;

use super::message::MessageScreen;

const GAP: i16 = 20;
const CARD_PADDING: i16 = 24;
const CARD_RADIUS: u8 = 24;
const SIDE_WIDTH: i16 = 480;
const DOT: i16 = 8;
const FIELD_PADDING: i16 = 18;
const PAIR_HEIGHT: i16 = 80;
const ADDRESS_MAX: usize = 63;
const CODE_DIGITS: usize = 6;
const CLOSE_KEY: u16 = 1;

pub struct PairDeviceScreen {
    address: TextBuffer,
    code: TextBuffer,
}

impl PairDeviceScreen {
    pub fn new() -> Self {
        Self { address: TextBuffer::new(), code: TextBuffer::new() }
    }

    fn ready(&self) -> bool {
        matches!(wifi::state(), WifiState::Connected { .. }) && valid_address(self.address.as_str()) && self.code.as_str().len() == CODE_DIGITS
    }

    fn pair(&mut self, cx: &mut Cx<Self>) {
        if !self.ready() {
            return;
        }
        let addr = String::from(self.address.as_str().trim());
        let code = String::from(self.code.as_str());
        self.code = TextBuffer::new();
        cx.focus(id!("pair_device"));
        cx.nav_of::<Overlay>().push(PairingScreen::new(addr, code));
    }

    fn info<'a>(&'a self, ui: &'a Ui<Msg>, landscape: bool) -> Element<'a, Msg> {
        let theme = ui.theme();
        let caption = theme.fonts.caption;
        let header = ui
            .row([ui.icon(icon(icons::LINK)).foreground(colors::CARD_ICON), ui.text(theme.fonts.title, "Pair with a six-digit code").foreground(colors::CARD_TITLE)])
            .gap(14)
            .align(Align::Center);
        let description = ui
            .text(caption, "On Android, open Developer options > Wireless debugging > Pair device with pairing code, then enter the pairing address and code shown there.")
            .foreground(colors::CARD_ICON)
            .wrap();
        let status = match wifi::state() {
            WifiState::Connected { ssid, .. } => ui
                .row([
                    ui.column([]).size(DOT, DOT).radius((DOT / 2) as u8).background(colors::ONLINE),
                    ui.text(caption, ui.format(format_args!("Connected to {}", wifi::ssid(&ssid)))).foreground(colors::ONLINE).ellipsis(Elide::End),
                ])
                .gap(10)
                .align(Align::Center),
            WifiState::Disabled => ui.text(caption, "Wi-Fi is off").foreground(colors::MUTED),
            _ => ui.text(caption, "Wi-Fi is not connected").foreground(colors::MUTED),
        };
        let device = ui
            .column([ui.text(caption, "This device").foreground(colors::MUTED), ui.text(theme.fonts.body, ui.store(&adb_link::host_name())), status])
            .gap(8);
        let body = if landscape {
            ui.column([header, description, ui.column([]).grow(1), divider(ui), device]).grow(1)
        } else {
            ui.column([header, description, divider(ui), device])
        };
        let info = card(ui, body.gap(16));
        if landscape { info.width(SIDE_WIDTH) } else { info }
    }

    fn form<'a>(&'a self, ui: &'a Ui<Msg>, landscape: bool) -> Element<'a, Msg> {
        let theme = ui.theme();
        let caption = theme.fonts.caption;
        let offline = !matches!(wifi::state(), WifiState::Connected { .. });
        let address = labeled(
            ui,
            "Pairing address",
            text_field(ui, &self.address, Msg::Address)
                .placeholder("192.168.x.x:xxxxx")
                .keyboard(&TOUCH_PAIR_ADDRESS_KEYBOARD)
                .padding_y(FIELD_PADDING)
                .disabled(offline)
                .id(id!("address")),
        );
        let code = labeled(
            ui,
            "Pairing code",
            text_field(ui, &self.code, Msg::Code)
                .placeholder("000000")
                .keyboard(&TOUCH_PAIR_CODE_KEYBOARD)
                .padding_y(FIELD_PADDING)
                .disabled(offline)
                .id(id!("code")),
        );
        let fields = if landscape { ui.row([address.grow(1), code.grow(1)]).gap(16) } else { ui.column([address, code]).gap(16) };
        let pair = button_from(ui.row([ui.text(theme.fonts.title, "Pair")]), theme.widgets.button.primary, Msg::Pair).height(PAIR_HEIGHT).disabled(!self.ready()).id(id!("pair"));
        let hint = ui.text(caption, "The pairing port differs from the connection port shown on the main Wireless debugging screen.").foreground(colors::MUTED).wrap();
        let form = card(ui, ui.column([fields, pair, hint]).gap(20));
        if landscape { form.grow(1) } else { form }
    }
}

fn valid_address(addr: &str) -> bool {
    let Some((host, port)) = addr.trim().rsplit_once(':') else { return false };
    !host.is_empty() && port.parse::<u16>().is_ok_and(|port| port > 0)
}

fn card<'a>(ui: &'a Ui<Msg>, content: Element<'a, Msg>) -> Element<'a, Msg> {
    ui.column([content]).padding(CARD_PADDING).radius(CARD_RADIUS).background(ui.colors().surface)
}

fn labeled<'a>(ui: &'a Ui<Msg>, label: &'a str, field: Element<'a, Msg>) -> Element<'a, Msg> {
    ui.column([ui.text(ui.theme().fonts.caption, label).foreground(colors::CARD_TITLE), field]).gap(12)
}

#[derive(Clone, Copy)]
pub enum Msg {
    Back,
    Address(TextInput),
    Code(TextInput),
    Pair,
}

impl Component for PairDeviceScreen {
    type Msg = Msg;

    fn update(&mut self, msg: Msg, cx: &mut Cx<Self>) {
        match msg {
            Msg::Back => cx.nav_of::<Root>().pop(),
            Msg::Address(TextInput::Enter) => cx.focus(id!("code")),
            Msg::Code(TextInput::Enter) | Msg::Pair => self.pair(cx),
            Msg::Address(TextInput::Custom(CLOSE_KEY)) | Msg::Code(TextInput::Custom(CLOSE_KEY)) => cx.focus(id!("pair_device")),
            Msg::Address(TextInput::Char(c)) if !(c.is_ascii_digit() || c == '.' || c == ':') || self.address.as_str().len() >= ADDRESS_MAX => {}
            Msg::Code(TextInput::Char(c)) if !c.is_ascii_digit() || self.code.as_str().len() >= CODE_DIGITS => {}
            Msg::Address(input) => {
                self.address.apply(input);
            }
            Msg::Code(input) => {
                self.code.apply(input);
            }
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let (width, height) = ui.screen_size();
        let landscape = width > height;
        let body = if landscape {
            ui.row([self.info(ui, true), self.form(ui, true)])
        } else {
            ui.column([self.info(ui, false), self.form(ui, false)])
        };
        ui.column([top_bar(ui, Some(Msg::Back), "Pair Device", ui.empty()), body.gap(GAP).padding_each(24, 0, 24, 24).grow(1)])
            .padding_each(0, ui.insets().top, 0, 0)
            .background(ui.colors().background)
            .focusable()
            .id(id!("pair_device"))
            .task(|this| wifi::follow(this, |_: &mut PairDeviceScreen| {}))
    }
}

impl Screen for PairDeviceScreen {}

/// Pairs while showing that it does, then tells how it went.
struct PairingScreen {
    addr: String,
    code: String,
    task: Option<Task>,
}

impl PairingScreen {
    fn new(addr: String, code: String) -> Self {
        Self { addr, code, task: None }
    }
}

async fn pair(this: This<PairingScreen>, addr: String, code: String) {
    let result = adb_link::pair(addr, code).await;
    this.with(|_, cx| {
        let message = match result {
            Ok(_) => MessageScreen::new("Pairing complete", "The ADB key is now authorized. Use the separate connection address shown on Android to connect."),
            Err(err) => MessageScreen::new("Pairing failed", err),
        };
        cx.nav_of::<Overlay>().push(message);
    });
}

#[derive(Clone, Copy)]
struct CancelPair;

impl Component for PairingScreen {
    type Msg = CancelPair;

    fn update(&mut self, _: CancelPair, cx: &mut Cx<Self>) {
        if let Some(task) = self.task.take() {
            task.abort();
        }
        cx.nav_of::<Overlay>().pop();
    }

    fn view<'a>(&'a self, ui: &'a Ui<CancelPair>) -> Element<'a, CancelPair> {
        let theme = ui.theme();
        modal(ui, [
            modal_title(ui, "Pairing"),
            ui.row([progress_indicator(ui, None), modal_message(ui, ui.format(format_args!("Pairing with {}…", self.addr))).grow(1)])
                .align(Align::Center)
                .gap(theme.widgets.unit.padding),
            modal_actions(ui, [button(ui, "Cancel", theme.widgets.button.secondary, CancelPair).id(id!("cancel"))]),
        ])
    }
}

impl Screen for PairingScreen {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        let (addr, code) = (self.addr.clone(), self.code.clone());
        self.task = Some(cx.spawn(move |this| pair(this, addr, code)));
    }
}
