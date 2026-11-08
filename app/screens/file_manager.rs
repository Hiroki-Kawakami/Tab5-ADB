/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::{String, ToString};
use alloc::vec::Vec;
use core::ffi::CStr;

use devkit_ui::widgets::{icon_button, menu, menu_item, progress_indicator, table, table_chevron, table_header, table_row, table_section, top_bar};
use devkit_ui::{Align, Child, CollectionLayout, Component, Cx, Elide, Element, Icon, Id, Justify, Popover, PopoverAlign, Prop, Root, Screen, Sides, SplitRole, State, Task, This, Ui, id};
use idf::log::error;

use crate::adb_link;
use crate::files::{self, Android, Entry, FileType, Kind, Volume};
use crate::fonts::{icon, small_icon};
use crate::resources::{colors, icons};

const TAG: &CStr = c"file_manager";
const SIDEBAR_WIDTH: i16 = 400;
const ROW_HEIGHT: i16 = 104;
const SPLIT_ROW_HEIGHT: i16 = 88;
const SPLIT_SECTION_PADDING: i16 = 8;
const FIRST_HEADER_TOP: i16 = 12;
const INTERNAL_ROOT: &str = "/sdcard";
const SYSTEM_ROOT: &str = "/";

/// Marks the navigator of the storage list and the folders.
pub struct Pane;

pub struct FileManagerScreen {
    android: bool,
}

impl FileManagerScreen {
    /// Without `android`, only the Tab5's own storage.
    pub fn new(android: bool) -> Self {
        Self { android }
    }
}

impl Component for FileManagerScreen {
    type Msg = ();

    fn update(&mut self, _: (), _: &mut Cx<Self>) {}

    fn view<'a>(&'a self, ui: &'a Ui<()>) -> Element<'a, ()> {
        let (width, height) = ui.screen_size();
        let pane = ui.navigator::<Pane, Storages>(self.android).split(width > height, SIDEBAR_WIDTH).grow(1).id(id!("pane"));
        ui.column([pane]).background(ui.colors().background)
    }
}

impl Screen for FileManagerScreen {}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Storage {
    Internal,
    System,
    Sd,
}

impl Storage {
    fn label(self) -> &'static str {
        match self {
            Storage::Internal => "Internal Storage",
            Storage::System => "System",
            Storage::Sd => "SD Card",
        }
    }

    fn glyph(self) -> char {
        match self {
            Storage::Internal => icons::PHONE,
            Storage::System => icons::STORAGE,
            Storage::Sd => icons::SD_CARD,
        }
    }
}

#[derive(Clone, Copy)]
pub enum StorageMsg {
    Back,
    Open(Storage),
}

pub struct Storages {
    android: Option<Android>,
    selected: Option<Storage>,
    internal_free: Option<String>,
    sd_space: String,
}

impl Storages {
    fn subtitle(&self, storage: Storage) -> &str {
        match storage {
            Storage::Internal => self.internal_free.as_deref().unwrap_or(INTERNAL_ROOT),
            Storage::System => "/ (root)",
            Storage::Sd => &self.sd_space,
        }
    }

    fn row<'a>(&'a self, ui: &'a Ui<StorageMsg>, storage: Storage) -> Element<'a, StorageMsg> {
        let theme = ui.theme();
        let text = ui.column([ui.text(theme.fonts.body, storage.label()), ui.text(theme.fonts.caption, self.subtitle(storage)).foreground(colors::MUTED)]).gap(4).grow(1);
        table_row(ui, [ui.icon(icon(storage.glyph())).foreground(colors::CARD_ICON), text, table_chevron(ui)]).height(ROW_HEIGHT).gap(20).on_tap(StorageMsg::Open(storage))
    }

    fn split_row<'a>(&'a self, ui: &'a Ui<StorageMsg>, storage: Storage, open: bool) -> Element<'a, StorageMsg> {
        let theme = ui.theme();
        let selected = open && self.selected == Some(storage);
        let subtitle = ui.text(theme.fonts.caption, self.subtitle(storage)).ellipsis(Elide::End).foreground(if selected { colors::ON_PRIMARY } else { colors::MUTED });
        let text = ui.column([ui.text(theme.fonts.body, storage.label()), subtitle]).gap(4).grow(1);
        let glyph = ui.icon(icon(storage.glyph())).foreground(if selected { colors::ON_PRIMARY } else { colors::CARD_ICON });
        let row = ui.row([glyph, text]).height(SPLIT_ROW_HEIGHT).padding_x(16).gap(16).align(Align::Center).radius(12).on_tap(StorageMsg::Open(storage));
        if selected {
            row.background(ui.colors().primary).foreground(colors::ON_PRIMARY)
        } else {
            row.when(State::PRESSED, &[Prop::Background(colors::PRESSED)])
        }
    }

    fn section<'a>(&'a self, ui: &'a Ui<StorageMsg>, storages: &[Storage]) -> Element<'a, StorageMsg> {
        match ui.split_role() {
            SplitRole::None => table_section(ui, storages.iter().map(|&s| self.row(ui, s).id(storage_id(s)))),
            role => {
                let open = matches!(role, SplitRole::Main { detail: true });
                let rows: Vec<_> = storages.iter().map(|&s| self.split_row(ui, s, open).id(storage_id(s))).collect();
                ui.column(rows).padding(SPLIT_SECTION_PADDING).gap(SPLIT_SECTION_PADDING).radius(24).background(ui.colors().surface)
            }
        }
    }
}

fn header<'a>(ui: &'a Ui<StorageMsg>, title: &'a str, first: bool) -> Element<'a, StorageMsg> {
    let header = table_header(ui, title);
    if first { header.padding_each(8, FIRST_HEADER_TOP, 8, ui.theme().widgets.unit.gap) } else { header }
}

fn storage_id(storage: Storage) -> Id {
    match storage {
        Storage::Internal => id!("internal"),
        Storage::System => id!("system"),
        Storage::Sd => id!("sd"),
    }
}

async fn load_internal_free(this: This<Storages>, android: Android) {
    let out = match adb::services::exec(android.connection(), "df -k /sdcard").await {
        Ok(out) => out,
        Err(err) => {
            error!(TAG, "df: {err}");
            return;
        }
    };
    let Some(space) = files::parse_df(&String::from_utf8_lossy(&out)) else { return };
    this.with(|s, cx| {
        s.internal_free = Some(alloc::format!("{INTERNAL_ROOT} \u{2022} {} free", files::format_bytes(space.free)));
        cx.rebuild();
    });
}

impl Component for Storages {
    type Msg = StorageMsg;
    type Props = bool;

    fn update(&mut self, msg: StorageMsg, cx: &mut Cx<Self>) {
        match msg {
            StorageMsg::Back => cx.nav_of::<Root>().pop(),
            StorageMsg::Open(storage) => {
                let volume = match (storage, &self.android) {
                    (Storage::Sd, _) => Volume::Sd,
                    (_, Some(android)) => Volume::Android(android.clone()),
                    (_, None) => return,
                };
                let root = match storage {
                    Storage::Internal => INTERNAL_ROOT,
                    Storage::System => SYSTEM_ROOT,
                    Storage::Sd => files::SD_ROOT,
                };
                self.selected = Some(storage);
                cx.nav().show(Browser::new(volume, storage.label(), root, root.to_string()));
            }
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<StorageMsg>) -> Element<'a, StorageMsg> {
        let mut sections = Vec::new();
        if self.android.is_some() {
            sections.push(header(ui, "ANDROID DEVICE", sections.is_empty()));
            sections.push(self.section(ui, &[Storage::Internal, Storage::System]));
        }
        if files::sd_present() {
            sections.push(header(ui, "M5STACK TAB5", sections.is_empty()));
            sections.push(self.section(ui, &[Storage::Sd]));
        }
        ui.column([top_bar(ui, Some(StorageMsg::Back), "File Manager", ui.empty()), table(ui, sections).grow(1)])
            .padding_each(0, ui.insets().top, 0, 0)
            .background(ui.colors().background)
    }
}

impl Screen for Storages {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        if let Some(android) = self.android.clone() {
            cx.spawn(|this| load_internal_free(this, android));
        }
        if files::sd_present() {
            self.sd_space = match files::sd_space() {
                Ok(space) => alloc::format!("{} free of {}", files::format_bytes(space.free), files::format_bytes(space.total)),
                Err(err) => err,
            };
        }
    }
}

impl Child for Storages {
    fn create(android: &bool) -> Self {
        let android = if *android { adb_link::connection().map(Android::new) } else { None };
        Self { android, selected: None, internal_free: None, sd_space: String::new() }
    }

    fn set_props(&mut self, _: &bool) {}
}

#[derive(Clone, Copy)]
pub enum Msg {
    Back,
    Open(u32),
    OpenMenu,
    Refresh,
}

/// One folder; a subfolder opens as another `Browser` above it.
pub struct Browser {
    volume: Volume,
    label: &'static str,
    root: &'static str,
    path: String,
    entries: Option<Result<Vec<Entry>, String>>,
    revision: u32,
    load: Option<Task>,
}

impl Browser {
    fn new(volume: Volume, label: &'static str, root: &'static str, path: String) -> Self {
        Self { volume, label, root, path, entries: None, revision: 0, load: None }
    }

    fn title(&self) -> &str {
        if self.path == self.root {
            return self.label;
        }
        self.path.rsplit('/').next().unwrap_or(self.label)
    }

    fn breadcrumb<'a>(&'a self, ui: &'a Ui<Msg>) -> &'a str {
        let below = self.path.strip_prefix(self.root).unwrap_or("").trim_start_matches('/');
        if below.is_empty() {
            return self.label;
        }
        let mut crumb = String::from(self.label);
        for part in below.split('/') {
            crumb.push_str(" \u{203a} ");
            crumb.push_str(part);
        }
        ui.store(&crumb)
    }

    fn entry(&self, index: usize) -> Option<&Entry> {
        self.entries.as_ref()?.as_ref().ok()?.get(index)
    }

    fn reload(&mut self, cx: &mut Cx<Self>) {
        if let Some(task) = self.load.take() {
            task.abort();
        }
        self.entries = None;
        let (volume, path) = (self.volume.clone(), self.path.clone());
        self.load = Some(cx.spawn(|this| load(this, volume, path)));
    }

    fn menu<'a>(_: &'a (), ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        menu(ui, [menu_item(ui, Some(small_icon(icons::REFRESH)), "Refresh", Msg::Refresh).id(id!("menu-refresh"))])
    }

    fn row<'a>(&'a self, index: usize, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let Some(entry) = self.entry(index) else { return ui.empty() };
        let theme = ui.theme();
        let chevron = if entry.kind == Kind::File { ui.empty() } else { ui.icon(theme.icons.chevron).foreground(colors::MUTED) };
        let line = ui
            .row([ui.icon(entry_icon(entry)).foreground(colors::CARD_ICON), ui.text(theme.fonts.body, &entry.name).ellipsis(Elide::End).grow(1), chevron])
            .gap(24)
            .align(Align::Center)
            .border(ui.colors().divider, 1)
            .border_sides(Sides::BOTTOM)
            .height(theme.widgets.list_item.height)
            .grow(1);
        let row = ui
            .row([line])
            .padding_x(24)
            .when(State::PRESSED, &[Prop::Background(colors::PRESSED)])
            .on_tap(Msg::Open(index as u32));
        if entry.kind == Kind::Link { row.task(move |this: This<Browser>| resolve(this, index)) } else { row }
    }

    fn content<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let theme = ui.theme();
        let message = |text: &'a str| ui.column([ui.text(theme.fonts.body, text).foreground(colors::MUTED).wrap().text_center()]).padding(24).align(Align::Center).justify(Justify::Center).grow(1);
        match &self.entries {
            None => ui.column([progress_indicator(ui, None)]).align(Align::Center).justify(Justify::Center).grow(1),
            Some(Err(err)) => message(err),
            Some(Ok(entries)) if entries.is_empty() => message("This folder is empty."),
            Some(Ok(entries)) => ui
                .collection(entries.len(), CollectionLayout::List { row_height: theme.widgets.list_item.height }, Self::row)
                .padding_each(0, 0, 0, ui.insets().bottom)
                .revision(self.revision)
                .id(id!("files"))
                .grow(1),
        }
    }
}

async fn load(this: This<Browser>, volume: Volume, path: String) {
    let entries = volume.list(&path).await;
    if let Err(err) = &entries {
        error!(TAG, "list {path}: {err}");
    }
    this.with(|s, cx| {
        s.entries = Some(entries);
        s.load = None;
        s.revision += 1;
        cx.rebuild();
    });
}

async fn resolve(this: This<Browser>, index: usize) {
    let Some((volume, name, path)) = this.with(|s, _| s.entry(index).map(|e| (s.volume.clone(), e.name.clone(), files::join(&s.path, &e.name)))).flatten() else { return };
    let Some(kind) = volume.resolve(&path).await else { return };
    this.with(|s, cx| {
        let Some(Ok(entries)) = &mut s.entries else { return };
        let Some(entry) = entries.get_mut(index).filter(|e| e.name == name) else { return };
        entry.kind = kind;
        s.revision += 1;
        cx.rebuild();
    });
}

impl Component for Browser {
    type Msg = Msg;

    fn update(&mut self, msg: Msg, cx: &mut Cx<Self>) {
        match msg {
            Msg::Back => cx.nav().pop(),
            Msg::Open(index) => {
                let Some(entry) = self.entry(index as usize) else { return };
                if entry.kind != Kind::File {
                    let path = files::join(&self.path, &entry.name);
                    cx.nav().push(Browser::new(self.volume.clone(), self.label, self.root, path));
                }
            }
            Msg::OpenMenu => {
                if let Some(anchor) = cx.tap_rect() {
                    cx.popup(Popover::new(anchor, (), Self::menu, cx.handle()).align(PopoverAlign::End));
                }
            }
            Msg::Refresh => self.reload(cx),
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let theme = ui.theme();
        let back = ui.can_pop().then_some(Msg::Back);
        let more = icon_button(ui, theme.icons.more, Msg::OpenMenu).id(id!("more"));
        let bar = if ui.split_role() == SplitRole::Detail { ui.colors().surface } else { ui.colors().background };
        ui.column([
            ui.column([top_bar(ui, back, self.title(), more)]).padding_each(0, ui.insets().top, 0, 0).background(bar),
            ui.text(theme.fonts.caption, self.breadcrumb(ui))
                .foreground(colors::MUTED)
                .ellipsis(Elide::Start)
                .height(48)
                .padding_each(24, 13, 24, 0)
                .border(ui.colors().divider, 1)
                .border_sides(Sides::TOP),
            self.content(ui),
        ])
        .background(ui.colors().surface)
    }
}

impl Screen for Browser {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        self.reload(cx);
    }
}

fn entry_icon(entry: &Entry) -> Icon {
    if entry.kind != Kind::File {
        return icon(icons::FOLDER_FILLED);
    }
    icon(match entry.file_type() {
        FileType::Apk => icons::ANDROID,
        FileType::Image => icons::PHOTO,
        FileType::Video => icons::MOVIE,
        FileType::Audio => icons::MUSIC,
        FileType::Text => icons::FILE_TEXT,
        FileType::Other => icons::FILE,
    })
}
