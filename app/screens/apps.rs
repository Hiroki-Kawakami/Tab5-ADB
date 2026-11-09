/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::boxed::Box;
use alloc::string::{String, ToString};
use alloc::sync::Arc;
use alloc::vec;
use alloc::vec::Vec;
use core::ffi::CStr;

use adb::link::Connection;
use devkit_ui::widgets::{divider, icon_button, menu, menu_divider, menu_item, progress_indicator, segmented, top_bar};
use devkit_ui::{Align, Child, CollectionLayout, ColorRole, Component, Cx, Elide, Element, Handle, Justify, Overlay, Part, Popover, PopoverAlign, Prop, Root, Screen, Sides, SplitRole, State, StateStyle, Task, This, Ui, id};
use idf::log::error;
use idf::sync::Mutex;

use crate::adb_link;
use crate::apps::{self, Action, App, Details};
use crate::file_ops::{self, Endpoint};
use crate::files::{self, FileType, Volume};
use crate::fonts::{icon, small_icon};
use crate::resources::{colors, icons};

use super::file_dialogs::{self, Choice, ProgressDialog};
use super::file_manager::{FileManagerScreen, FilePick};
use super::message::MessageScreen;

const TAG: &CStr = c"apps";
const LIST_WIDTH: i16 = 560;
const ROW_HEIGHT: i16 = 104;
const SPLIT_ROW_HEIGHT: i16 = 96;
const ROW_ICON_SIZE: i16 = 64;
const ROW_ICON_RADIUS: u8 = 14;
const BADGE_SIZE: i16 = 96;
const SPLIT_BADGE_SIZE: i16 = 72;
const BADGE_RADIUS: u8 = 20;
const TAB_HEIGHT: i16 = 56;
const ACTION_HEIGHT: i16 = 80;
const INFO_ROW_HEIGHT: i16 = 64;
const MARGIN: i16 = 24;
const GAP: i16 = 20;
const RADIUS: u8 = 24;
const FADED: u8 = 102;
const BUSY: &str = "Another file operation is running.";

static TAB_TRACK: StateStyle = StateStyle {
    font: None,
    base: &[Prop::Background(colors::PRESSED)],
    states: &[],
};

static TAB: StateStyle = StateStyle {
    font: None,
    base: &[Prop::Height(TAB_HEIGHT), Prop::Foreground(colors::CARD_ICON)],
    states: &[(State::SELECTED, &[Prop::Background(ColorRole::Surface), Prop::Foreground(ColorRole::Text), Prop::Shadow(1, 4, colors::SHADOW)])],
};

static TONAL: StateStyle = StateStyle {
    font: None,
    base: &[Prop::Background(ColorRole::Background), Prop::Foreground(ColorRole::Text), Prop::Radius(14)],
    states: &[(State::PRESSED, &[Prop::Background(colors::PRESSED)]), (State::DISABLED, &[Prop::Opacity(FADED)])],
};

/// Marks the navigator of the app list and an app's details.
struct Pane;

pub struct AppsScreen;

impl AppsScreen {
    pub fn new() -> Self {
        Self
    }
}

impl Component for AppsScreen {
    type Msg = ();

    fn update(&mut self, _: (), _: &mut Cx<Self>) {}

    fn view<'a>(&'a self, ui: &'a Ui<()>) -> Element<'a, ()> {
        let (width, height) = ui.screen_size();
        let pane = ui.navigator::<Pane, AppList>(()).split(width > height, LIST_WIDTH).grow(1).id(id!("pane"));
        ui.column([pane]).background(ui.colors().background)
    }
}

impl Screen for AppsScreen {}

fn app_icon<'a, M: Copy>(ui: &'a Ui<M>, size: i16, radius: u8) -> Element<'a, M> {
    ui.row([ui.icon(icon(icons::ANDROID)).foreground(colors::ANDROID)]).size(size, size).radius(radius).align(Align::Center).justify(Justify::Center).background(colors::ANDROID_BADGE).shrink(0)
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Group {
    User,
    System,
}

#[derive(Clone, Copy)]
pub enum ListMsg {
    Back,
    Tab(Group),
    Open(u32),
    Refresh,
    Reload,
    Install,
    Picked,
    InstallConfirmed,
}

pub struct AppList {
    conn: Option<Connection>,
    user: Vec<App>,
    system: Vec<App>,
    error: Option<String>,
    loaded: bool,
    group: Group,
    selected: Option<String>,
    revision: u32,
    load: Option<Task>,
    picked: Option<Arc<Mutex<Option<(Volume, String)>>>>,
    install: Option<(Volume, String)>,
}

impl AppList {
    fn apps(&self) -> &[App] {
        match self.group {
            Group::User => &self.user,
            Group::System => &self.system,
        }
    }

    /// Without `clear`, the list stays until the new one comes.
    fn reload(&mut self, clear: bool, cx: &mut Cx<Self>) {
        if let Some(task) = self.load.take() {
            task.abort();
        }
        let Some(conn) = self.conn.clone() else {
            self.error = Some("Not connected.".to_string());
            self.loaded = true;
            return;
        };
        if clear {
            self.loaded = false;
            self.error = None;
        }
        self.load = Some(cx.spawn(|this| load_list(this, conn)));
    }

    fn pick_apk(&mut self, cx: &mut Cx<Self>) {
        let result = Arc::new(Mutex::new(None));
        let handle = cx.handle();
        let done = Box::new(move || {
            if !handle.post(ListMsg::Picked) {
                error!(TAG, "picked: UI queue full");
            }
        });
        self.picked = Some(result.clone());
        cx.nav_of::<Root>().push(FileManagerScreen::pick_file(true, FilePick { file_type: FileType::Apk, result, done }));
    }

    fn confirm_install(&mut self, cx: &mut Cx<Self>) {
        let Some(picked) = self.picked.take().and_then(|picked| picked.lock().take()) else { return };
        let message = alloc::format!("\"{}\" will be installed.", files::name_of(&picked.1));
        self.install = Some(picked);
        let buttons = vec![("Cancel", file_dialogs::Action::Secondary, None), ("Install", file_dialogs::Action::Primary, Some(ListMsg::InstallConfirmed))];
        let handle = cx.handle();
        cx.nav_of::<Overlay>().push(Choice::new("Install APK", message, buttons, handle));
    }

    fn install(&mut self, cx: &mut Cx<Self>) {
        let (Some((volume, path)), Some(conn)) = (self.install.take(), self.conn.clone()) else { return };
        let name = files::name_of(&path).to_string();
        let Some(op) = file_ops::install(Endpoint::from(&volume), conn, path) else {
            cx.nav_of::<Overlay>().push(MessageScreen::new("Cannot Install", BUSY));
            return;
        };
        let handle = cx.handle();
        let done = move || {
            if !handle.post(ListMsg::Reload) {
                error!(TAG, "reload: UI queue full");
            }
        };
        cx.nav_of::<Overlay>().push(ProgressDialog::new("Installing", "Install failed", op).on_success("Installed", alloc::format!("{name} was installed.")).on_done(done));
    }

    fn header<'a>(&'a self, ui: &'a Ui<ListMsg>) -> Element<'a, ListMsg> {
        let actions = ui.row([
            icon_button(ui, icon(icons::INSTALL), ListMsg::Install).disabled(self.conn.is_none()).id(id!("install")),
            icon_button(ui, icon(icons::REFRESH), ListMsg::Refresh).id(id!("refresh")),
        ]);
        let tabs = [(Group::User, "User", self.user.len()), (Group::System, "System", self.system.len())].map(|(group, label, count)| {
            let label = if self.loaded && self.error.is_none() { ui.format(format_args!("{label} ({count})")) } else { label };
            (label, ListMsg::Tab(group))
        });
        let selected = usize::from(self.group == Group::System);
        let body = ui.theme().fonts.body;
        let tabs = segmented(ui, &tabs, selected).style(&TAB_TRACK).part(Part::Item, |item| item.style(&TAB).width(0).grow(1)).part(Part::Label, |label| label.font(body)).grow(1);
        let bar = if ui.split_role() == SplitRole::None { ui.colors().background } else { ui.colors().surface };
        ui.column([top_bar(ui, Some(ListMsg::Back), "Apps", actions), ui.row([tabs]).padding_each(MARGIN, 0, MARGIN, 16)])
            .padding_each(0, ui.insets().top, 0, 0)
            .background(bar)
            .border(ui.colors().divider, 1)
            .border_sides(Sides::BOTTOM)
    }

    fn row<'a>(&'a self, index: usize, ui: &'a Ui<ListMsg>) -> Element<'a, ListMsg> {
        let Some(app) = self.apps().get(index) else { return ui.empty() };
        let theme = ui.theme();
        let split = ui.split_role() != SplitRole::None;
        let selected = matches!(ui.split_role(), SplitRole::Main { detail: true }) && self.selected.as_deref() == Some(app.package.as_str());
        let badge = if app.disabled {
            ui.row([ui.text(theme.fonts.caption, "Disabled")]).height(36).padding_x(12).radius(8).align(Align::Center).background(colors::PRESSED).foreground(colors::MUTED).shrink(0)
        } else {
            ui.empty()
        };
        let chevron = if split { ui.empty() } else { ui.icon(theme.icons.chevron).foreground(colors::MUTED) };
        let name = ui.text(theme.fonts.body, &app.package).ellipsis(Elide::End).grow(1);
        let (tile, name) = if app.disabled { (app_icon(ui, ROW_ICON_SIZE, ROW_ICON_RADIUS).opacity(FADED), name.foreground(colors::MUTED)) } else { (app_icon(ui, ROW_ICON_SIZE, ROW_ICON_RADIUS), name) };
        let line = ui.row([tile, name, badge, chevron]).gap(20).align(Align::Center).border(ui.colors().divider, 1).border_sides(Sides::BOTTOM).grow(1);
        let row = ui.row([line]).padding_x(MARGIN).on_tap(ListMsg::Open(index as u32));
        if selected { row.background(colors::SELECTED) } else { row.when(State::PRESSED, &[Prop::Background(colors::PRESSED)]) }
    }

    fn content<'a>(&'a self, ui: &'a Ui<ListMsg>) -> Element<'a, ListMsg> {
        let theme = ui.theme();
        let message = |text: &'a str| ui.column([ui.text(theme.fonts.body, text).foreground(colors::MUTED).wrap().text_center()]).padding(24).align(Align::Center).justify(Justify::Center).grow(1);
        if let Some(err) = &self.error {
            return message(err);
        }
        if !self.loaded {
            return ui.column([progress_indicator(ui, None)]).align(Align::Center).justify(Justify::Center).grow(1);
        }
        let apps = self.apps();
        if apps.is_empty() {
            return message("No apps.");
        }
        let row_height = if ui.split_role() == SplitRole::None { ROW_HEIGHT } else { SPLIT_ROW_HEIGHT };
        let list = ui.collection(apps.len(), CollectionLayout::List { row_height }, Self::row).revision(self.revision);
        match self.group {
            Group::User => list.id(id!("user-apps")),
            Group::System => list.id(id!("system-apps")),
        }
        .grow(1)
    }
}

async fn load_list(this: This<AppList>, conn: Connection) {
    let result = apps::list(&conn).await;
    if let Err(err) = &result {
        error!(TAG, "list: {err}");
    }
    this.with(|s, cx| {
        match result {
            Ok(apps) => {
                (s.system, s.user) = apps.into_iter().partition(|app| app.system);
                s.error = None;
            }
            Err(err) => s.error = Some(err),
        }
        s.loaded = true;
        s.load = None;
        s.revision += 1;
        cx.rebuild();
    });
}

impl Component for AppList {
    type Msg = ListMsg;

    fn update(&mut self, msg: ListMsg, cx: &mut Cx<Self>) {
        match msg {
            ListMsg::Back => cx.nav_of::<Root>().pop(),
            ListMsg::Tab(group) => {
                self.group = group;
                self.revision += 1;
            }
            ListMsg::Open(index) => {
                if let (Some(app), Some(conn)) = (self.apps().get(index as usize).cloned(), self.conn.clone()) {
                    self.selected = Some(app.package.clone());
                    let handle = cx.handle();
                    cx.nav().show(AppDetail::new(conn, app, handle));
                }
            }
            ListMsg::Refresh => self.reload(true, cx),
            ListMsg::Reload => self.reload(false, cx),
            ListMsg::Install => self.pick_apk(cx),
            ListMsg::Picked => self.confirm_install(cx),
            ListMsg::InstallConfirmed => self.install(cx),
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<ListMsg>) -> Element<'a, ListMsg> {
        ui.column([self.header(ui), self.content(ui), ui.column([]).height(ui.insets().bottom)]).background(ui.colors().surface)
    }
}

impl Screen for AppList {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        self.reload(true, cx);
    }
}

impl Child for AppList {
    fn create(_: &()) -> Self {
        Self {
            conn: adb_link::connection(),
            user: Vec::new(),
            system: Vec::new(),
            error: None,
            loaded: false,
            group: Group::User,
            selected: None,
            revision: 0,
            load: None,
            picked: None,
            install: None,
        }
    }

    fn set_props(&mut self, _: &()) {}
}

#[derive(Clone, Copy)]
pub enum DetailMsg {
    Back,
    OpenMenu,
    Ask(Action),
    Run(Action),
}

/// One app's details and what can be done to it.
pub struct AppDetail {
    conn: Connection,
    app: App,
    details: Option<Result<Details, String>>,
    running: bool,
    list: Handle<ListMsg>,
}

impl AppDetail {
    fn new(conn: Connection, app: App, list: Handle<ListMsg>) -> Self {
        Self { conn, app, details: None, running: false, list }
    }

    fn menu<'a>(app: &'a (bool, bool), ui: &'a Ui<DetailMsg>) -> Element<'a, DetailMsg> {
        let &(system, disabled) = app;
        let toggle = if disabled {
            menu_item(ui, Some(small_icon(icons::CIRCLE_CHECK)), "Enable", DetailMsg::Run(Action::Enable)).id(id!("menu-enable"))
        } else {
            menu_item(ui, Some(small_icon(icons::BAN)), "Disable", DetailMsg::Ask(Action::Disable)).id(id!("menu-disable"))
        };
        let uninstall = if system { ui.empty() } else { menu_item(ui, Some(small_icon(icons::TRASH)), "Uninstall", DetailMsg::Ask(Action::Uninstall)).foreground(colors::DESTRUCTIVE).id(id!("menu-uninstall")) };
        menu(ui, [
            toggle,
            menu_divider(ui),
            menu_item(ui, Some(small_icon(icons::ERASER)), "Clear Data", DetailMsg::Ask(Action::ClearData)).foreground(colors::DESTRUCTIVE).id(id!("menu-clear")),
            uninstall,
        ])
    }

    fn ask(&mut self, action: Action, cx: &mut Cx<Self>) {
        let package = &self.app.package;
        let (title, message, label) = match action {
            Action::Disable => ("Disable App", alloc::format!("Disable \"{package}\"? It stops running and disappears from the launcher until it is enabled again."), "Disable"),
            Action::ClearData => ("Clear Data", alloc::format!("Delete all data of \"{package}\"?\nThis can't be undone."), "Clear"),
            Action::Uninstall => ("Uninstall", alloc::format!("Uninstall \"{package}\"?\nIts data is deleted too."), "Uninstall"),
            _ => return self.run(action, cx),
        };
        let buttons = vec![("Cancel", file_dialogs::Action::Secondary, None), (label, file_dialogs::Action::Destructive, Some(DetailMsg::Run(action)))];
        let handle = cx.handle();
        cx.nav_of::<Overlay>().push(Choice::new(title, message, buttons, handle));
    }

    fn run(&mut self, action: Action, cx: &mut Cx<Self>) {
        if self.running {
            return;
        }
        self.running = true;
        let (conn, package) = (self.conn.clone(), self.app.package.clone());
        cx.spawn(|this: This<Self>| async move {
            let result = apps::run(&conn, action, &package).await;
            if let Err(err) = &result {
                error!(TAG, "{action:?} {package}: {err}");
            }
            this.with(|s, cx| {
                s.running = false;
                match result {
                    Ok(()) => s.done(action, cx),
                    Err(err) => cx.nav_of::<Overlay>().push(MessageScreen::new(action.failure(), err)),
                }
                cx.rebuild();
            });
        });
    }

    fn done(&mut self, action: Action, cx: &mut Cx<Self>) {
        match action {
            Action::Disable | Action::Enable => self.app.disabled = action == Action::Disable,
            Action::Uninstall => cx.nav().root(),
            _ => return,
        }
        if !self.list.post(ListMsg::Reload) {
            error!(TAG, "reload: UI queue full");
        }
    }

    fn head<'a>(&'a self, ui: &'a Ui<DetailMsg>) -> Element<'a, DetailMsg> {
        let theme = ui.theme();
        let size = if ui.split_role() == SplitRole::Detail { SPLIT_BADGE_SIZE } else { BADGE_SIZE };
        let title = ui.text(theme.fonts.title, &self.app.package).wrap().grow(1);
        let action = |glyph: char, label: &'a str, style: &'static StateStyle, msg: DetailMsg| {
            ui.row([ui.icon(small_icon(glyph)), ui.text(theme.fonts.body, label)]).style(style).height(ACTION_HEIGHT).width(0).grow(1).gap(12).align(Align::Center).justify(Justify::Center).on_tap(msg)
        };
        let actions = ui.row([
            action(icons::PLAY, "Launch", theme.widgets.button.primary, DetailMsg::Run(Action::Launch)).disabled(self.app.disabled || self.running).id(id!("launch")),
            action(icons::STOP, "Force Stop", &TONAL, DetailMsg::Run(Action::ForceStop)).disabled(self.running).id(id!("force-stop")),
        ]);
        ui.column([ui.row([app_icon(ui, size, BADGE_RADIUS), title]).gap(MARGIN).align(Align::Center), actions.gap(16)]).gap(GAP).padding(MARGIN).radius(RADIUS).background(ui.colors().surface)
    }

    fn info<'a>(&'a self, ui: &'a Ui<DetailMsg>) -> Element<'a, DetailMsg> {
        let theme = ui.theme();
        if let Some(Err(err)) = &self.details {
            return ui.text(theme.fonts.body, err).foreground(colors::MUTED).wrap().padding(MARGIN).radius(RADIUS).background(ui.colors().surface);
        }
        let (version, installed, updated, path) = match &self.details {
            Some(Ok(d)) => (d.version.as_str(), d.installed.as_str(), d.updated.as_str(), d.path.as_str()),
            _ => ("", "", "", ""),
        };
        let status = if self.app.disabled { "Disabled" } else { "Enabled" };
        let (width, height) = ui.screen_size();
        let path = if width > height { inline(ui, "Path", path) } else { stacked(ui, "Path", path) };
        let rows = vec![
            inline(ui, "Version", version),
            divider(ui),
            inline(ui, "Status", status),
            divider(ui),
            inline(ui, "Installed", installed),
            divider(ui),
            inline(ui, "Updated", updated),
            divider(ui),
            path,
        ];
        ui.column(rows).padding_x(MARGIN).padding_y(4).radius(RADIUS).background(ui.colors().surface)
    }
}

fn inline<'a>(ui: &'a Ui<DetailMsg>, label: &'a str, value: &'a str) -> Element<'a, DetailMsg> {
    let theme = ui.theme();
    ui.row([ui.text(theme.fonts.caption, label).foreground(colors::MUTED).shrink(0), ui.text(theme.fonts.body, value).text_end().ellipsis(Elide::Start).grow(1)]).height(INFO_ROW_HEIGHT).gap(MARGIN).align(Align::Center)
}

fn stacked<'a>(ui: &'a Ui<DetailMsg>, label: &'a str, value: &'a str) -> Element<'a, DetailMsg> {
    let theme = ui.theme();
    ui.column([ui.text(theme.fonts.caption, label).foreground(colors::MUTED), ui.text(theme.fonts.body, value).wrap()]).gap(4).padding_y(16)
}

async fn load_details(this: This<AppDetail>, conn: Connection, package: String) {
    let details = apps::details(&conn, &package).await;
    if let Err(err) = &details {
        error!(TAG, "details {package}: {err}");
    }
    this.with(|s, cx| {
        s.details = Some(details);
        cx.rebuild();
    });
}

impl Component for AppDetail {
    type Msg = DetailMsg;

    fn update(&mut self, msg: DetailMsg, cx: &mut Cx<Self>) {
        match msg {
            DetailMsg::Back => cx.nav().pop(),
            DetailMsg::OpenMenu => {
                if let Some(anchor) = cx.tap_rect() {
                    cx.popup(Popover::new(anchor, (self.app.system, self.app.disabled), Self::menu, cx.handle()).align(PopoverAlign::End));
                }
            }
            DetailMsg::Ask(action) => self.ask(action, cx),
            DetailMsg::Run(action) => self.run(action, cx),
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<DetailMsg>) -> Element<'a, DetailMsg> {
        let theme = ui.theme();
        let back = ui.can_pop().then_some(DetailMsg::Back);
        let more = icon_button(ui, theme.icons.more, DetailMsg::OpenMenu).disabled(self.running).id(id!("more"));
        let body = ui.column([self.head(ui), self.info(ui)]).gap(GAP).padding_each(MARGIN, 4, MARGIN, MARGIN + ui.insets().bottom);
        ui.column([top_bar(ui, back, "App Info", more), ui.scroll(body).grow(1)]).padding_each(0, ui.insets().top, 0, 0).background(ui.colors().background)
    }
}

impl Screen for AppDetail {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        let (conn, package) = (self.conn.clone(), self.app.package.clone());
        cx.spawn(|this| load_details(this, conn, package));
    }
}
