/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;
use core::cmp::Reverse;
use core::ffi::CStr;
use core::time::Duration;

use devkit_net::Security;
use devkit_net::wifi_manager::{JoinError, Manager, NetworkInfo, ScanEntry, ScanError, State as WifiState};
use devkit_ui::widgets::{button, icon_button, modal, modal_actions, modal_message, modal_title, password_field, progress_indicator, switch, table, table_chevron, table_header, table_label, table_row, table_section, table_value, text_field, top_bar};
use devkit_ui::{Align, Component, Cx, Element, Elide, Overlay, Screen, Task, TextBuffer, TextInput, This, Ui, id};
use idf::log::error;

use crate::fonts::{icon, small_icon};
use crate::resources::{colors, icons};
use crate::wifi;

use super::file_dialogs::{Action, Choice};
use super::message::MessageScreen;

const TAG: &CStr = c"wifi_screen";
const SCAN_INTERVAL: Duration = Duration::from_secs(10);
const SECTION_GAP: i16 = 32;
const STATUS_GAP: i16 = 8;
const DOT: i16 = 8;

fn connected_ssid() -> Option<Vec<u8>> {
    match wifi::state() {
        WifiState::Connected { ssid, .. } => Some(ssid),
        _ => None,
    }
}

fn joining(ssid: &[u8]) -> bool {
    matches!(wifi::state(), WifiState::Joining { ssid: s } if s == ssid)
}

fn spacer<'a, M: Copy>(ui: &'a Ui<M>) -> Element<'a, M> {
    ui.column([]).height(SECTION_GAP)
}

fn detail<'a, M: Copy>(ui: &'a Ui<M>, label: &'a str, value: &'a str) -> Element<'a, M> {
    let caption = ui.theme().fonts.caption;
    ui.row([ui.text(caption, label).foreground(colors::MUTED).grow(1), ui.text(caption, value)]).align(Align::Center)
}

#[derive(Clone, Copy)]
pub enum Msg {
    Back,
    Toggle,
    Refresh,
    Network(u8),
    Saved,
    Add,
}

pub struct WifiScreen {
    enabled: bool,
    networks: Vec<ScanEntry>,
    saved: Vec<Vec<u8>>,
}

impl WifiScreen {
    pub fn new() -> Self {
        let mut screen = Self { enabled: false, networks: Vec::new(), saved: Vec::new() };
        screen.refresh();
        screen
    }

    fn refresh(&mut self) {
        let Some(manager) = wifi::manager() else { return };
        self.enabled = manager.enabled();
        self.saved = manager.networks().into_iter().map(|n| n.ssid).collect();
        if !self.enabled {
            self.networks.clear();
        }
        self.sort();
    }

    fn sort(&mut self) {
        let connected = connected_ssid();
        for entry in &mut self.networks {
            entry.connected = connected.as_deref() == Some(&entry.ssid[..]);
            entry.saved = self.saved.contains(&entry.ssid);
        }
        self.networks.sort_by_key(|e| (Reverse(e.connected), Reverse(e.saved), Reverse(e.rssi)));
    }

    fn scanned(&mut self, result: Result<Vec<ScanEntry>, ScanError>) {
        match result {
            Ok(mut networks) => {
                networks.retain(|e| !e.ssid.is_empty());
                networks.truncate(usize::from(u8::MAX));
                self.networks = networks;
                self.sort();
            }
            Err(ScanError::Disabled) => {}
            Err(err) => error!(TAG, "scan: {err:?}"),
        }
    }

    fn open(&mut self, index: usize, cx: &mut Cx<Self>) {
        let Some(entry) = self.networks.get(index) else { return };
        if entry.connected || !entry.security.is_supported() {
            return;
        }
        let ssid = entry.ssid.clone();
        if !entry.saved && entry.security != Security::Open {
            cx.nav_of::<Overlay>().push(JoinDialog::connect(ssid, None));
            return;
        }
        let join = if entry.saved { Join::Saved } else { Join::Open };
        cx.nav_of::<Overlay>().push(JoiningScreen::new(ssid, join));
    }

    fn status<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let toggle = table_row(ui, [table_label(ui, "Wi-Fi"), switch(ui, self.enabled, Msg::Toggle).id(id!("toggle"))]);
        if !self.enabled {
            return table_section(ui, [toggle]);
        }
        let caption = ui.theme().fonts.caption;
        let mut lines = Vec::new();
        match wifi::state() {
            WifiState::Connected { ssid, .. } => {
                let text = ui.format(format_args!("Connected to {}", wifi::ssid(&ssid)));
                lines.push(
                    ui.row([ui.column([]).size(DOT, DOT).radius((DOT / 2) as u8).background(colors::ONLINE), ui.text(caption, text).foreground(colors::ONLINE).ellipsis(Elide::End)])
                        .gap(10)
                        .align(Align::Center),
                );
                let ip = wifi::ipv4();
                lines.push(detail(ui, "IP Address", ip.as_deref().map_or("Obtaining…", |ip| ui.store(ip))));
                if let Some(mac) = wifi::mac() {
                    lines.push(detail(ui, "MAC Address", ui.store(&mac)));
                }
            }
            WifiState::Joining { ssid } => lines.push(ui.text(caption, ui.format(format_args!("Connecting to {}…", wifi::ssid(&ssid)))).foreground(colors::MUTED)),
            WifiState::Scanning => lines.push(ui.text(caption, "Searching…").foreground(colors::MUTED)),
            _ => lines.push(ui.text(caption, "Not connected").foreground(colors::MUTED)),
        }
        let padding = ui.theme().widgets.unit.padding;
        let status = ui.column(lines).gap(STATUS_GAP).padding_each(padding, padding, padding, padding).id(id!("status"));
        table_section(ui, [toggle, status])
    }

    fn network<'a>(&'a self, ui: &'a Ui<Msg>, index: usize, entry: &'a ScanEntry) -> Element<'a, Msg> {
        let theme = ui.theme();
        let primary = ui.colors().primary;
        let supported = entry.security.is_supported();
        let sub = if entry.connected {
            Some("Connected")
        } else if joining(&entry.ssid) {
            Some("Connecting…")
        } else if entry.saved {
            Some("Saved")
        } else if !supported {
            Some("Not supported")
        } else {
            None
        };
        let ssid = ui.text(theme.fonts.body, ui.store(&wifi::ssid(&entry.ssid))).ellipsis(Elide::End);
        let ssid = if entry.connected { ssid.foreground(primary) } else { ssid };
        let sub = sub.map_or(ui.empty(), |sub| {
            let sub = ui.text(theme.fonts.caption, sub);
            if entry.connected { sub.foreground(primary) } else { sub.foreground(colors::MUTED) }
        });
        let strength = ui.icon(small_icon(wifi::strength_glyph(Some(entry.rssi))));
        let strength = if entry.connected { strength.foreground(primary) } else { strength.foreground(colors::CARD_ICON) };
        let lock = if entry.security == Security::Open { ui.empty() } else { ui.icon(small_icon(icons::LOCK)).foreground(colors::MUTED) };
        table_row(ui, [strength, ui.column([ssid, sub]).gap(2).grow(1), lock]).disabled(!supported).on_tap(Msg::Network(index as u8))
    }
}

async fn scan(this: This<WifiScreen>) {
    let Some(manager) = wifi::manager() else { return };
    let mut watch = manager.watch_scan(SCAN_INTERVAL);
    loop {
        let result = watch.changed().await;
        if this.with(|s, _| s.scanned(result)).is_none() {
            return;
        }
    }
}

impl Component for WifiScreen {
    type Msg = Msg;

    fn update(&mut self, msg: Msg, cx: &mut Cx<Self>) {
        match msg {
            Msg::Back => cx.nav().pop(),
            Msg::Toggle => {
                let Some(manager) = wifi::manager() else { return };
                self.enabled = !self.enabled;
                if !self.enabled {
                    self.networks.clear();
                }
                let enabled = self.enabled;
                cx.spawn(move |this| async move {
                    if let Err(err) = manager.set_enabled(enabled).await {
                        error!(TAG, "set_enabled: {err}");
                    }
                    this.with(|s, _| s.refresh());
                });
            }
            Msg::Refresh => {
                let Some(manager) = wifi::manager() else { return };
                cx.spawn(move |this| async move {
                    let result = manager.scan().await;
                    this.with(|s, _| s.scanned(result));
                });
            }
            Msg::Network(index) => self.open(usize::from(index), cx),
            Msg::Saved => cx.nav().push(SavedNetworksScreen::new()),
            Msg::Add => cx.nav_of::<Overlay>().push(JoinDialog::add()),
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let refresh = icon_button(ui, icon(icons::REFRESH), Msg::Refresh).disabled(!self.enabled).id(id!("refresh"));
        let mut children = vec![self.status(ui)];
        if self.enabled {
            let rows: Vec<_> = if self.networks.is_empty() {
                vec![table_row(ui, [ui.text(ui.theme().fonts.body, "Searching…").foreground(colors::MUTED)])]
            } else {
                self.networks.iter().enumerate().map(|(i, entry)| self.network(ui, i, entry)).collect()
            };
            children.push(table_header(ui, "NETWORKS"));
            children.push(table_section(ui, rows).id(id!("networks")).task(scan));
            children.push(spacer(ui));
            let count = ui.format(format_args!("{}", self.saved.len()));
            children.push(table_section(ui, [
                table_row(ui, [table_label(ui, "Saved Networks"), table_value(ui, count), table_chevron(ui)]).on_tap(Msg::Saved).id(id!("saved")),
                table_row(ui, [table_label(ui, "Add Network"), table_chevron(ui)]).on_tap(Msg::Add).id(id!("add")),
            ]));
        }
        ui.column([top_bar(ui, Some(Msg::Back), "Wi-Fi", refresh).scroll_border(id!("content")), table(ui, children).grow(1).id(id!("content"))])
            .padding_each(0, ui.insets().top, 0, 0)
            .background(ui.colors().background)
            .task(|this| wifi::follow(this, WifiScreen::refresh))
    }
}

impl Screen for WifiScreen {}

#[derive(Clone, Copy)]
pub enum SavedMsg {
    Back,
    Open(u8),
    Connect,
    Disconnect,
    Forget,
}

pub struct SavedNetworksScreen {
    networks: Vec<NetworkInfo>,
    connected: Option<Vec<u8>>,
    selected: Option<Vec<u8>>,
}

impl SavedNetworksScreen {
    fn new() -> Self {
        let mut screen = Self { networks: Vec::new(), connected: None, selected: None };
        screen.refresh();
        screen
    }

    fn refresh(&mut self) {
        self.networks = wifi::manager().map(|m| m.networks()).unwrap_or_default();
        self.connected = connected_ssid();
    }

    fn row<'a>(&'a self, ui: &'a Ui<SavedMsg>, index: usize, network: &'a NetworkInfo) -> Element<'a, SavedMsg> {
        let theme = ui.theme();
        let connected = self.connected.as_ref() == Some(&network.ssid);
        let ssid = ui.text(theme.fonts.body, ui.store(&wifi::ssid(&network.ssid))).ellipsis(Elide::End);
        let sub = if connected {
            ui.text(theme.fonts.caption, "Connected").foreground(ui.colors().primary)
        } else if joining(&network.ssid) {
            ui.text(theme.fonts.caption, "Connecting…").foreground(colors::MUTED)
        } else if network.hidden {
            ui.text(theme.fonts.caption, "Hidden").foreground(colors::MUTED)
        } else {
            ui.empty()
        };
        let ssid = if connected { ssid.foreground(ui.colors().primary) } else { ssid };
        let lock = if network.secured { ui.icon(small_icon(icons::LOCK)).foreground(colors::MUTED) } else { ui.empty() };
        table_row(ui, [ui.column([ssid, sub]).gap(2).grow(1), lock]).on_tap(SavedMsg::Open(index as u8))
    }
}

impl Component for SavedNetworksScreen {
    type Msg = SavedMsg;

    fn update(&mut self, msg: SavedMsg, cx: &mut Cx<Self>) {
        let Some(manager) = wifi::manager() else { return };
        match msg {
            SavedMsg::Back => cx.nav().pop(),
            SavedMsg::Open(index) => {
                let Some(network) = self.networks.get(usize::from(index)) else { return };
                let connected = self.connected.as_ref() == Some(&network.ssid);
                let primary = if connected { ("Disconnect", Action::Primary, Some(SavedMsg::Disconnect)) } else { ("Connect", Action::Primary, Some(SavedMsg::Connect)) };
                let buttons = vec![("Cancel", Action::Secondary, None), ("Forget", Action::Destructive, Some(SavedMsg::Forget)), primary];
                self.selected = Some(network.ssid.clone());
                let choice = Choice::new("Saved Network", wifi::ssid(&network.ssid), buttons, cx.handle());
                cx.nav_of::<Overlay>().push(choice);
            }
            SavedMsg::Connect => {
                let Some(ssid) = self.selected.take() else { return };
                cx.nav_of::<Overlay>().push(JoiningScreen::new(ssid, Join::Saved));
            }
            SavedMsg::Disconnect => {
                self.selected = None;
                cx.spawn(move |this| async move {
                    manager.disconnect().await;
                    this.with(|s, _| s.refresh());
                });
            }
            SavedMsg::Forget => {
                let Some(ssid) = self.selected.take() else { return };
                cx.spawn(move |this| async move {
                    if let Err(err) = manager.forget(&ssid).await {
                        error!(TAG, "forget: {err}");
                    }
                    this.with(|s, _| s.refresh());
                });
            }
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<SavedMsg>) -> Element<'a, SavedMsg> {
        let content = if self.networks.is_empty() {
            ui.text(ui.theme().fonts.body, "No saved networks.").foreground(colors::MUTED).padding_each(8, SECTION_GAP, 8, 0)
        } else {
            let rows: Vec<_> = self.networks.iter().enumerate().map(|(i, network)| self.row(ui, i, network)).collect();
            ui.column([spacer(ui), table_section(ui, rows)])
        };
        ui.column([top_bar(ui, Some(SavedMsg::Back), "Saved Networks", ui.empty()).scroll_border(id!("content")), table(ui, [content]).grow(1).id(id!("content"))])
            .padding_each(0, ui.insets().top, 0, 0)
            .background(ui.colors().background)
            .task(|this| wifi::follow(this, SavedNetworksScreen::refresh))
    }
}

impl Screen for SavedNetworksScreen {}

/// Asks for the password of `ssid`, or for a network by name, and joins it.
pub struct JoinDialog {
    ssid: Option<Vec<u8>>,
    name: TextBuffer,
    password: TextBuffer,
    shown: bool,
    error: Option<String>,
}

impl JoinDialog {
    fn connect(ssid: Vec<u8>, error: Option<String>) -> Self {
        Self { ssid: Some(ssid), name: TextBuffer::new(), password: TextBuffer::new(), shown: false, error }
    }

    fn add() -> Self {
        Self { ssid: None, name: TextBuffer::new(), password: TextBuffer::new(), shown: false, error: None }
    }

    fn retry(ssid: Option<Vec<u8>>, name: &str, password: &str, error: String) -> Self {
        let mut dialog = Self { ssid, name: TextBuffer::new(), password: TextBuffer::new(), shown: false, error: Some(error) };
        dialog.name.set_text(name);
        dialog.password.set_text(password);
        dialog
    }

    fn submit(&mut self, cx: &mut Cx<Self>) {
        let (ssid, hidden) = match &self.ssid {
            Some(ssid) => (ssid.clone(), false),
            None => (self.name.as_str().trim().as_bytes().to_vec(), true),
        };
        let join = Join::Dialog { password: String::from(self.password.as_str()), hidden, ssid_known: self.ssid.is_some() };
        cx.nav_of::<Overlay>().push(JoiningScreen::new(ssid, join));
    }
}

#[derive(Clone, Copy)]
pub enum JoinMsg {
    Name(TextInput),
    Password(TextInput),
    Reveal,
    Cancel,
    Submit,
}

impl Component for JoinDialog {
    type Msg = JoinMsg;

    fn update(&mut self, msg: JoinMsg, cx: &mut Cx<Self>) {
        match msg {
            JoinMsg::Name(TextInput::Enter) => cx.focus(id!("password")),
            JoinMsg::Name(input) => {
                self.name.apply(input);
                self.error = None;
            }
            JoinMsg::Password(TextInput::Enter) | JoinMsg::Submit => self.submit(cx),
            JoinMsg::Password(input) => {
                self.password.apply(input);
                self.error = None;
            }
            JoinMsg::Reveal => self.shown = !self.shown,
            JoinMsg::Cancel => cx.nav_of::<Overlay>().pop(),
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<JoinMsg>) -> Element<'a, JoinMsg> {
        let b = &ui.theme().widgets.button;
        let (title, name) = match &self.ssid {
            Some(ssid) => (ui.format(format_args!("Connect to {}", wifi::ssid(ssid))), ui.empty()),
            None => ("Add Network", text_field(ui, &self.name, JoinMsg::Name).placeholder("Network name").id(id!("name"))),
        };
        let placeholder = if self.ssid.is_some() { "Password" } else { "Password (none for an open network)" };
        let error = match &self.error {
            Some(err) => modal_message(ui, err).foreground(colors::DESTRUCTIVE).id(id!("error")),
            None => ui.empty(),
        };
        let ready = self.ssid.is_some() || !self.name.as_str().trim().is_empty();
        modal(ui, [
            modal_title(ui, title),
            name,
            password_field(ui, &self.password, self.shown, JoinMsg::Password, JoinMsg::Reveal).placeholder(placeholder).id(id!("password")),
            error,
            modal_actions(ui, [
                button(ui, "Cancel", b.secondary, JoinMsg::Cancel).id(id!("cancel")),
                button(ui, "Connect", b.primary, JoinMsg::Submit).disabled(!ready).id(id!("connect")),
            ]),
        ])
    }
}

impl Screen for JoinDialog {
    fn will_appear(&mut self, cx: &mut Cx<Self>) {
        cx.focus(if self.ssid.is_some() { id!("password") } else { id!("name") });
    }

    fn scrim_tap(&self) -> Option<JoinMsg> {
        Some(JoinMsg::Cancel)
    }
}

enum Join {
    Open,
    Saved,
    Dialog { password: String, hidden: bool, ssid_known: bool },
}

/// Joins `ssid` while showing that it does; a failure goes back to where a
/// password can be given, or tells why.
pub struct JoiningScreen {
    ssid: Vec<u8>,
    join: Join,
    task: Option<Task>,
}

impl JoiningScreen {
    fn new(ssid: Vec<u8>, join: Join) -> Self {
        Self { ssid, join, task: None }
    }

    fn failed(&self, err: JoinError, cx: &mut Cx<Self>) {
        let message = wifi::join_error(err);
        match &self.join {
            Join::Dialog { password, ssid_known, .. } => {
                let (ssid, name) = if *ssid_known { (Some(self.ssid.clone()), String::new()) } else { (None, wifi::ssid(&self.ssid)) };
                cx.nav_of::<Overlay>().push(JoinDialog::retry(ssid, &name, password, message));
            }
            Join::Saved if err == JoinError::AuthFailed => cx.nav_of::<Overlay>().push(JoinDialog::connect(self.ssid.clone(), Some(message))),
            _ => cx.nav_of::<Overlay>().push(MessageScreen::new("Connection failed", message)),
        }
    }
}

async fn join(this: This<JoiningScreen>, manager: &'static Manager, ssid: Vec<u8>, password: Option<String>, hidden: bool) {
    let result = match &password {
        Some(password) => manager.join(&ssid, password, hidden).await,
        None => manager.join_saved(&ssid).await,
    };
    this.with(|s, cx| match result {
        Ok(()) => cx.nav_of::<Overlay>().pop(),
        Err(err) => s.failed(err, cx),
    });
}

#[derive(Clone, Copy)]
pub struct CancelJoin;

impl Component for JoiningScreen {
    type Msg = CancelJoin;

    fn update(&mut self, _: CancelJoin, cx: &mut Cx<Self>) {
        if let Some(task) = self.task.take() {
            task.abort();
        }
        cx.nav_of::<Overlay>().pop();
    }

    fn view<'a>(&'a self, ui: &'a Ui<CancelJoin>) -> Element<'a, CancelJoin> {
        let theme = ui.theme();
        let message = ui.format(format_args!("Connecting to {}…", wifi::ssid(&self.ssid)));
        modal(ui, [
            modal_title(ui, "Connecting"),
            ui.row([progress_indicator(ui, None), modal_message(ui, message).grow(1)]).align(Align::Center).gap(theme.widgets.unit.padding),
            modal_actions(ui, [button(ui, "Cancel", theme.widgets.button.secondary, CancelJoin).id(id!("cancel"))]),
        ])
    }
}

impl Screen for JoiningScreen {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        let Some(manager) = wifi::manager() else {
            cx.nav_of::<Overlay>().pop();
            return;
        };
        let ssid = self.ssid.clone();
        let (password, hidden) = match &self.join {
            Join::Open => (Some(String::new()), false),
            Join::Saved => (None, false),
            Join::Dialog { password, hidden, .. } => (Some(password.clone()), *hidden),
        };
        self.task = Some(cx.spawn(move |this| join(this, manager, ssid, password, hidden)));
    }
}
