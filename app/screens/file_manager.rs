/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::boxed::Box;
use alloc::rc::Rc;
use alloc::string::{String, ToString};
use alloc::sync::Arc;
use alloc::vec;
use alloc::vec::Vec;
use core::cell::RefCell;
use core::ffi::CStr;
use core::sync::atomic::{AtomicU32, Ordering};

use devkit_ui::widgets::{button, icon_button, menu, menu_item, progress_indicator, table, table_chevron, table_header, table_row, table_section, top_bar};
use devkit_ui::{Align, Child, CollectionLayout, ColorRole, Component, Cx, Elide, Element, Handle, Icon, Id, Justify, Overlay, Popover, PopoverAlign, Prop, Root, Screen, Sides, SplitRole, State, StateStyle, Task, This, Ui, id};
use idf::log::error;
use idf::sync::Mutex;

use crate::adb_link;
use crate::file_ops::{self, Copy, Endpoint};
use crate::files::{self, Android, Entry, FileRef, FileType, Kind, Volume};
use crate::fonts::{icon, small_icon};
use crate::resources::{colors, icons};

use super::file_dialogs::{Action, Choice, NameDialog, Naming, ProgressDialog};
use super::file_preview::FilePreviewScreen;
use super::message::MessageScreen;

const TAG: &CStr = c"file_manager";
const SIDEBAR_WIDTH: i16 = 400;
const ROW_HEIGHT: i16 = 104;
const SPLIT_ROW_HEIGHT: i16 = 88;
const SPLIT_SECTION_PADDING: i16 = 8;
const FIRST_HEADER_TOP: i16 = 12;
const BAR_BUTTON_HEIGHT: i16 = 56;
const TOOL_HEIGHT: i16 = 88;
const INTERNAL_ROOT: &str = "/sdcard";
const SYSTEM_ROOT: &str = "/";
const BUSY: &str = "Another file operation is running.";

static TONAL: StateStyle = StateStyle {
    font: None,
    base: &[Prop::Background(ColorRole::Background), Prop::Foreground(ColorRole::Text), Prop::Radius(14)],
    states: &[(State::PRESSED, &[Prop::Background(colors::PRESSED)]), (State::DISABLED, &[Prop::Opacity(102)])],
};

static BAR_TONAL: StateStyle = StateStyle {
    font: None,
    base: &[Prop::Background(colors::PRESSED), Prop::Foreground(ColorRole::Text), Prop::Radius(14)],
    states: &[(State::PRESSED, &[Prop::Background(ColorRole::Divider)])],
};

static DESTRUCTIVE_TONAL: StateStyle = StateStyle {
    font: None,
    base: &[Prop::Background(colors::DESTRUCTIVE_TONAL), Prop::Foreground(colors::DESTRUCTIVE), Prop::Radius(14)],
    states: &[(State::PRESSED, &[Prop::Background(colors::PRESSED)]), (State::DISABLED, &[Prop::Opacity(102)])],
};

static CHANGES: AtomicU32 = AtomicU32::new(0);

/// Folders shown before a change load again when they show.
pub fn changed() {
    CHANGES.fetch_add(1, Ordering::AcqRel);
}

/// Marks the navigator of the storage list and the folders.
pub struct Pane;

/// A folder to copy one item named `name` into. Once one is chosen, `result`
/// holds the volume and the path the item gets there, and `done` runs.
pub struct FolderPick {
    pub name: String,
    pub result: Arc<Mutex<Option<(Volume, String)>>>,
    pub done: Box<dyn Fn() + Send>,
}

/// A file of `file_type` to open. Once one is chosen, `result` holds its
/// volume and path, and `done` runs.
pub struct FilePick {
    pub file_type: FileType,
    pub result: Arc<Mutex<Option<(Volume, String)>>>,
    pub done: Box<dyn Fn() + Send>,
}

enum Picking {
    Folder(FolderPick),
    File(FilePick),
}

static PENDING_PICK: Mutex<Option<Picking>> = Mutex::new(None);

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Mode {
    android: bool,
    pick: bool,
}

pub struct FileManagerScreen {
    mode: Mode,
}

impl FileManagerScreen {
    /// Without `android`, only the Tab5's own storage.
    pub fn new(android: bool) -> Self {
        Self { mode: Mode { android, pick: false } }
    }

    /// Browses for a folder instead; closes once one is chosen or on Cancel.
    pub fn pick_folder(android: bool, pick: FolderPick) -> Self {
        *PENDING_PICK.lock() = Some(Picking::Folder(pick));
        Self { mode: Mode { android, pick: true } }
    }

    /// Browses for a file instead; closes once one is chosen or on Cancel.
    pub fn pick_file(android: bool, pick: FilePick) -> Self {
        *PENDING_PICK.lock() = Some(Picking::File(pick));
        Self { mode: Mode { android, pick: true } }
    }
}

impl Component for FileManagerScreen {
    type Msg = ();

    fn update(&mut self, _: (), _: &mut Cx<Self>) {}

    fn view<'a>(&'a self, ui: &'a Ui<()>) -> Element<'a, ()> {
        let (width, height) = ui.screen_size();
        let pane = ui.navigator::<Pane, Storages>(self.mode).split(width > height, SIDEBAR_WIDTH).grow(1).id(id!("pane"));
        ui.column([pane]).background(ui.colors().background)
    }
}

impl Screen for FileManagerScreen {}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Transfer {
    Move,
    Copy,
}

/// Items picked for Move or Copy, until they are pasted or dropped.
pub struct Clipboard {
    transfer: Transfer,
    volume: Volume,
    dir: String,
    items: Vec<Entry>,
}

#[derive(Clone)]
struct Shared {
    clipboard: Rc<RefCell<Option<Clipboard>>>,
    storages: Handle<StorageMsg>,
    pick: Option<Rc<Picking>>,
}

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

    fn kind(self) -> files::Storage {
        match self {
            Storage::Internal | Storage::System => files::Storage::Android,
            Storage::Sd => files::Storage::Sd,
        }
    }
}

#[derive(Clone, Copy)]
pub enum StorageMsg {
    Back,
    Open(Storage),
    Clipboard,
}

pub struct Storages {
    android: Option<Android>,
    selected: Option<Storage>,
    internal_free: Option<String>,
    sd_space: String,
    clipboard: Rc<RefCell<Option<Clipboard>>>,
    pick: Option<Rc<Picking>>,
}

impl Storages {
    fn subtitle(&self, storage: Storage) -> &str {
        match storage {
            Storage::Internal => self.internal_free.as_deref().unwrap_or(INTERNAL_ROOT),
            Storage::System => "/ (root)",
            Storage::Sd => &self.sd_space,
        }
    }

    /// Moving keeps to the storage it started on.
    fn reachable(&self, storage: Storage) -> bool {
        match &*self.clipboard.borrow() {
            Some(clip) if clip.transfer == Transfer::Move => clip.volume.storage() == storage.kind(),
            _ => true,
        }
    }

    fn row<'a>(&'a self, ui: &'a Ui<StorageMsg>, storage: Storage) -> Element<'a, StorageMsg> {
        let theme = ui.theme();
        let text = ui.column([ui.text(theme.fonts.body, storage.label()), ui.text(theme.fonts.caption, self.subtitle(storage)).foreground(colors::MUTED)]).gap(4).grow(1);
        table_row(ui, [ui.icon(icon(storage.glyph())).foreground(colors::CARD_ICON), text, table_chevron(ui)])
            .height(ROW_HEIGHT)
            .gap(20)
            .on_tap(StorageMsg::Open(storage))
            .disabled(!self.reachable(storage))
    }

    fn split_row<'a>(&'a self, ui: &'a Ui<StorageMsg>, storage: Storage, open: bool) -> Element<'a, StorageMsg> {
        let theme = ui.theme();
        let selected = open && self.selected == Some(storage);
        let subtitle = ui.text(theme.fonts.caption, self.subtitle(storage)).ellipsis(Elide::End).foreground(if selected { colors::ON_PRIMARY } else { colors::MUTED });
        let text = ui.column([ui.text(theme.fonts.body, storage.label()), subtitle]).gap(4).grow(1);
        let glyph = ui.icon(icon(storage.glyph())).foreground(if selected { colors::ON_PRIMARY } else { colors::CARD_ICON });
        let row = ui.row([glyph, text]).height(SPLIT_ROW_HEIGHT).padding_x(16).gap(16).align(Align::Center).radius(12).on_tap(StorageMsg::Open(storage));
        let row = if selected {
            row.background(ui.colors().primary).foreground(colors::ON_PRIMARY)
        } else {
            row.when(State::PRESSED, &[Prop::Background(colors::PRESSED)])
        };
        row.when(State::DISABLED, &[Prop::Opacity(102)]).disabled(!self.reachable(storage))
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
    type Props = Mode;

    fn update(&mut self, msg: StorageMsg, cx: &mut Cx<Self>) {
        match msg {
            StorageMsg::Back => cx.nav_of::<Root>().pop(),
            StorageMsg::Open(storage) => {
                if !self.reachable(storage) {
                    return;
                }
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
                let shared = Shared { clipboard: self.clipboard.clone(), storages: cx.handle(), pick: self.pick.clone() };
                cx.nav().show(Browser::new(volume, storage.label(), root, root.to_string(), shared));
            }
            StorageMsg::Clipboard => {}
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
    fn create(mode: &Mode) -> Self {
        let android = if mode.android { adb_link::connection().map(Android::new) } else { None };
        let pick = if mode.pick { PENDING_PICK.lock().take().map(Rc::new) } else { None };
        Self { android, selected: None, internal_free: None, sd_space: String::new(), clipboard: Rc::new(RefCell::new(None)), pick }
    }

    fn set_props(&mut self, _: &Mode) {}
}

#[derive(Clone, Copy)]
pub enum Msg {
    Back,
    Open(u32),
    OpenMenu,
    Refresh,
    Reload,
    Select,
    NewFolder,
    SelectAll,
    Done,
    Move,
    Copy,
    Rename,
    Delete,
    DeleteConfirmed,
    CancelPaste,
    Paste,
    PasteSkip,
    PasteKeepBoth,
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
    loaded: u32,
    selection: Option<Vec<bool>>,
    shared: Shared,
}

impl Browser {
    fn new(volume: Volume, label: &'static str, root: &'static str, path: String, shared: Shared) -> Self {
        Self { volume, label, root, path, entries: None, revision: 0, load: None, loaded: 0, selection: None, shared }
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

    fn entries(&self) -> &[Entry] {
        match &self.entries {
            Some(Ok(entries)) => entries,
            _ => &[],
        }
    }

    fn entry(&self, index: usize) -> Option<&Entry> {
        self.entries().get(index)
    }

    fn selected(&self) -> Vec<&Entry> {
        let Some(selection) = &self.selection else { return Vec::new() };
        self.entries().iter().zip(selection).filter(|(_, on)| **on).map(|(e, _)| e).collect()
    }

    fn selected_count(&self) -> usize {
        self.selection.as_ref().map_or(0, |s| s.iter().filter(|&&on| on).count())
    }

    fn all_selected(&self) -> bool {
        self.selection.as_ref().is_some_and(|s| !s.is_empty() && s.iter().all(|&on| on))
    }

    fn fold_case(&self) -> bool {
        self.volume.storage() == files::Storage::Sd
    }

    fn taken(&self, name: &str) -> bool {
        self.entries().iter().any(|e| if self.fold_case() { e.name.eq_ignore_ascii_case(name) } else { e.name == name })
    }

    fn reload(&mut self, cx: &mut Cx<Self>) {
        if let Some(task) = self.load.take() {
            task.abort();
        }
        self.entries = None;
        self.selection = None;
        self.loaded = CHANGES.load(Ordering::Acquire);
        let (volume, path) = (self.volume.clone(), self.path.clone());
        self.load = Some(cx.spawn(|this| load(this, volume, path)));
    }

    fn end_selection(&mut self) {
        self.selection = None;
        self.revision += 1;
    }

    fn clipboard_changed(&self, cx: &mut Cx<Self>) {
        cx.send_to(&self.shared.storages, StorageMsg::Clipboard);
    }

    fn menu<'a>(picking: &'a bool, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let select = if *picking { ui.empty() } else { menu_item(ui, Some(small_icon(icons::CIRCLE_CHECK)), "Select", Msg::Select).id(id!("menu-select")) };
        menu(ui, [
            select,
            menu_item(ui, Some(small_icon(icons::FOLDER_PLUS)), "New Folder", Msg::NewFolder).id(id!("menu-new-folder")),
            menu_item(ui, Some(small_icon(icons::REFRESH)), "Refresh", Msg::Refresh).id(id!("menu-refresh")),
        ])
    }

    fn row<'a>(&'a self, index: usize, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let Some(entry) = self.entry(index) else { return ui.empty() };
        let theme = ui.theme();
        let checked = self.selection.as_ref().map(|s| s.get(index).copied().unwrap_or(false));
        let trailing = match checked {
            Some(true) => ui.icon(small_icon(icons::CIRCLE_CHECK)).foreground(ColorRole::Primary),
            Some(false) => ui.icon(small_icon(icons::CIRCLE)).foreground(colors::MUTED),
            None if entry.kind == Kind::File => ui.empty(),
            None => ui.icon(theme.icons.chevron).foreground(colors::MUTED),
        };
        let line = ui
            .row([ui.icon(entry_icon(entry)).foreground(colors::CARD_ICON), ui.text(theme.fonts.body, &entry.name).ellipsis(Elide::End).grow(1), trailing])
            .gap(24)
            .align(Align::Center)
            .border(ui.colors().divider, 1)
            .border_sides(Sides::BOTTOM)
            .height(theme.widgets.list_item.height)
            .grow(1);
        let row = ui.row([line]).padding_x(24).on_tap(Msg::Open(index as u32));
        let row = if !self.openable(entry) {
            row.when(State::DISABLED, &[Prop::Opacity(102)]).disabled(true)
        } else if checked == Some(true) {
            row.background(colors::SELECTED)
        } else {
            row.when(State::PRESSED, &[Prop::Background(colors::PRESSED)])
        };
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
                .revision(self.revision)
                .id(id!("files"))
                .grow(1),
        }
    }

    fn header<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let theme = ui.theme();
        if self.selection.is_some() {
            let count = self.selected_count();
            let tonal = if ui.split_role() == SplitRole::Detail { &TONAL } else { &BAR_TONAL };
            let toggle = if self.all_selected() { "Deselect All" } else { "Select All" };
            return ui
                .row([
                    ui.text(theme.fonts.title, ui.format(format_args!("{count} selected"))).ellipsis(Elide::End).grow(1),
                    ui.row([ui.text(theme.fonts.caption, toggle)]).style(tonal).height(BAR_BUTTON_HEIGHT).padding_x(20).align(Align::Center).on_tap(Msg::SelectAll).id(id!("select-all")),
                    ui.row([ui.text(theme.fonts.caption, "Done")]).style(theme.widgets.button.primary).height(BAR_BUTTON_HEIGHT).padding_x(28).align(Align::Center).on_tap(Msg::Done).id(id!("done")),
                ])
                .height(80)
                .padding_each(28, 0, 24, 0)
                .gap(12)
                .align(Align::Center);
        }
        let back = ui.can_pop().then_some(Msg::Back);
        let more = icon_button(ui, theme.icons.more, Msg::OpenMenu).id(id!("more"));
        top_bar(ui, back, self.title(), more)
    }

    fn tools<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let count = self.selected_count();
        let tool = |glyph: char, label: &'a str, msg: Msg, style: &'static StateStyle, enabled: bool, id: Id| {
            ui.column([ui.icon(small_icon(glyph)), ui.text(ui.theme().fonts.caption, label)])
                .style(style)
                .height(TOOL_HEIGHT)
                .width(0)
                .grow(1)
                .gap(6)
                .align(Align::Center)
                .justify(Justify::Center)
                .on_tap(msg)
                .disabled(!enabled)
                .id(id)
        };
        ui.row([
            tool(icons::MOVE, "Move", Msg::Move, &TONAL, count > 0, id!("move")),
            tool(icons::COPY, "Copy", Msg::Copy, &TONAL, count > 0, id!("copy")),
            tool(icons::PENCIL, "Rename", Msg::Rename, &TONAL, count == 1, id!("rename")),
            tool(icons::TRASH, "Delete", Msg::Delete, &DESTRUCTIVE_TONAL, count > 0, id!("delete")),
        ])
        .gap(12)
        .padding_each(24, 16, 24, 16 + ui.insets().bottom)
        .border(ui.colors().divider, 1)
        .border_sides(Sides::TOP)
    }

    /// Whether the clipboard's items may land in this folder.
    fn can_paste(&self, clip: &Clipboard) -> bool {
        if self.entries.as_ref().is_none_or(Result::is_err) {
            return false;
        }
        if clip.transfer == Transfer::Move && (clip.volume.storage() != self.volume.storage() || clip.dir == self.path) {
            return false;
        }
        clip.volume.storage() != self.volume.storage() || !clip.items.iter().any(|item| item.kind != Kind::File && files::is_within(&self.path, &files::join(&clip.dir, &item.name)))
    }

    fn paste_bar<'a>(&'a self, ui: &'a Ui<Msg>, transfer: Transfer, count: usize, enabled: bool) -> Element<'a, Msg> {
        let theme = ui.theme();
        let b = &theme.widgets.button;
        let (verb, action) = match transfer {
            Transfer::Move => ("Move", "Move Here"),
            Transfer::Copy => ("Copy", "Copy Here"),
        };
        let what = if count == 1 { ui.format(format_args!("{verb} 1 item")) } else { ui.format(format_args!("{verb} {count} items")) };
        ui.row([
            ui.column([ui.text(theme.fonts.body, what), ui.text(theme.fonts.caption, "Open the destination folder").foreground(colors::MUTED).ellipsis(Elide::End)]).gap(4).grow(1),
            ui.row([ui.text(theme.fonts.body, "Cancel")]).style(&TONAL).height(72).padding_x(24).align(Align::Center).on_tap(Msg::CancelPaste).id(id!("cancel-paste")),
            button(ui, action, b.primary, Msg::Paste).padding_x(28).disabled(!enabled).id(id!("paste")),
        ])
        .gap(12)
        .align(Align::Center)
        .padding_each(24, 16, 24, 16 + ui.insets().bottom)
        .border(ui.colors().divider, 1)
        .border_sides(Sides::TOP)
    }

    fn openable(&self, entry: &Entry) -> bool {
        match self.shared.pick.as_deref() {
            Some(Picking::Folder(_)) => entry.kind != Kind::File,
            Some(Picking::File(pick)) => entry.kind != Kind::File || entry.file_type() == pick.file_type,
            None => true,
        }
    }

    fn file_pick_bar<'a>(&'a self, ui: &'a Ui<Msg>, file_type: FileType) -> Element<'a, Msg> {
        let theme = ui.theme();
        ui.row([
            ui.column([ui.text(theme.fonts.body, "Choose a File"), ui.text(theme.fonts.caption, files::type_label(file_type)).foreground(colors::MUTED).ellipsis(Elide::End)]).gap(4).grow(1),
            ui.row([ui.text(theme.fonts.body, "Cancel")]).style(&TONAL).height(72).padding_x(24).align(Align::Center).on_tap(Msg::CancelPaste).id(id!("cancel-paste")),
        ])
        .gap(12)
        .align(Align::Center)
        .padding_each(24, 16, 24, 16 + ui.insets().bottom)
        .border(ui.colors().divider, 1)
        .border_sides(Sides::TOP)
    }

    fn finish_file_pick(&self, path: String, cx: &mut Cx<Self>) {
        let Some(Picking::File(pick)) = self.shared.pick.as_deref() else { return };
        *pick.result.lock() = Some((self.volume.clone(), path));
        (pick.done)();
        cx.nav_of::<Root>().pop();
    }

    fn start_pick(&mut self, cx: &mut Cx<Self>) {
        let Some(rc) = self.shared.pick.clone() else { return };
        let Picking::Folder(pick) = &*rc else { return };
        if !matches!(self.entries, Some(Ok(_))) {
            return;
        }
        if !self.taken(&pick.name) {
            self.finish_pick(cx);
            return;
        }
        let message = alloc::format!("An item named \"{}\" already exists in \"{}\".", pick.name, self.title());
        let buttons = vec![("Cancel", Action::Secondary, None), ("Skip", Action::Secondary, Some(Msg::PasteSkip)), ("Keep Both", Action::Primary, Some(Msg::PasteKeepBoth))];
        let handle = cx.handle();
        cx.nav_of::<Overlay>().push(Choice::new("Item Already Exists", message, buttons, handle));
    }

    fn finish_pick(&mut self, cx: &mut Cx<Self>) {
        let Some(rc) = self.shared.pick.clone() else { return };
        let Picking::Folder(pick) = &*rc else { return };
        let name = files::unused_name(&pick.name, false, |name| self.taken(name));
        *pick.result.lock() = Some((self.volume.clone(), files::join(&self.path, &name)));
        (pick.done)();
        cx.nav_of::<Root>().pop();
    }

    fn start_paste(&mut self, cx: &mut Cx<Self>) {
        let existing: Vec<String> = {
            let clipboard = self.shared.clipboard.borrow();
            let Some(clip) = clipboard.as_ref().filter(|clip| self.can_paste(clip)) else { return };
            clip.items.iter().filter(|item| self.taken(&item.name)).map(|item| item.name.clone()).collect()
        };
        if existing.is_empty() {
            self.paste(false, cx);
            return;
        }
        let place = self.title();
        let (title, message) = if existing.len() == 1 {
            ("Item Already Exists", alloc::format!("An item named \"{}\" already exists in \"{place}\".", existing[0]))
        } else {
            ("Items Already Exist", alloc::format!("{} items with the same names already exist in \"{place}\".", existing.len()))
        };
        let buttons = vec![("Cancel", Action::Secondary, None), ("Skip", Action::Secondary, Some(Msg::PasteSkip)), ("Keep Both", Action::Primary, Some(Msg::PasteKeepBoth))];
        let handle = cx.handle();
        cx.nav_of::<Overlay>().push(Choice::new(title, message, buttons, handle));
    }

    fn paste(&mut self, skip_existing: bool, cx: &mut Cx<Self>) {
        let Some(clip) = self.shared.clipboard.borrow_mut().take() else { return };
        self.clipboard_changed(cx);
        let mut names: Vec<String> = self.entries().iter().map(|e| e.name.clone()).collect();
        let fold = self.fold_case();
        let mut pairs = Vec::new();
        for item in &clip.items {
            let taken = |name: &str| names.iter().any(|n| if fold { n.eq_ignore_ascii_case(name) } else { n == name });
            if taken(&item.name) && skip_existing {
                continue;
            }
            let name = files::unused_name(&item.name, item.kind != Kind::File, taken);
            pairs.push((files::join(&clip.dir, &item.name), files::join(&self.path, &name)));
            names.push(name);
        }
        if pairs.is_empty() {
            return;
        }
        match clip.transfer {
            Transfer::Copy => {
                let copies = pairs.into_iter().map(|(source, target)| Copy { source, target }).collect();
                match file_ops::copy(Endpoint::from(&clip.volume), Endpoint::from(&self.volume), copies) {
                    Some(op) => {
                        let handle = cx.handle();
                        cx.nav_of::<Overlay>().push(ProgressDialog::new("Copying", "Copy failed", op).on_done(move || reload(&handle)))
                    }
                    None => cx.nav_of::<Overlay>().push(MessageScreen::new("Cannot Copy", BUSY)),
                }
            }
            Transfer::Move => {
                let volume = self.volume.clone();
                cx.spawn(|this| async move {
                    let mut failure = None;
                    for (source, target) in &pairs {
                        if let Err(err) = volume.rename(source, target).await {
                            failure = Some(alloc::format!("{}\n{err}", files::name_of(source)));
                            break;
                        }
                    }
                    changed();
                    this.with(|s, cx| {
                        s.reload(cx);
                        if let Some(failure) = failure {
                            cx.nav_of::<Overlay>().push(MessageScreen::new("Move failed", failure));
                        }
                    });
                });
            }
        }
    }

    fn delete(&mut self, cx: &mut Cx<Self>) {
        let paths: Vec<String> = self.selected().iter().map(|e| files::join(&self.path, &e.name)).collect();
        if paths.is_empty() {
            return;
        }
        match file_ops::delete(Endpoint::from(&self.volume), paths) {
            Some(op) => {
                self.end_selection();
                let handle = cx.handle();
                cx.nav_of::<Overlay>().push(ProgressDialog::new("Deleting", "Delete failed", op).on_done(move || reload(&handle)));
            }
            None => cx.nav_of::<Overlay>().push(MessageScreen::new("Cannot Delete", BUSY)),
        }
    }

    fn confirm_delete(&mut self, cx: &mut Cx<Self>) {
        let selected = self.selected();
        let target = match selected.as_slice() {
            [] => return,
            [one] => alloc::format!("\"{}\"", one.name),
            many => alloc::format!("{} items", many.len()),
        };
        let message = alloc::format!("Are you sure you want to delete {target}?\nThis can't be undone.");
        let buttons = vec![("Cancel", Action::Secondary, None), ("Delete", Action::Destructive, Some(Msg::DeleteConfirmed))];
        let handle = cx.handle();
        cx.nav_of::<Overlay>().push(Choice::new("Delete Permanently", message, buttons, handle));
    }

    fn take_to_clipboard(&mut self, transfer: Transfer, cx: &mut Cx<Self>) {
        let items: Vec<Entry> = self.selected().into_iter().cloned().collect();
        if items.is_empty() {
            return;
        }
        *self.shared.clipboard.borrow_mut() = Some(Clipboard { transfer, volume: self.volume.clone(), dir: self.path.clone(), items });
        self.end_selection();
        self.clipboard_changed(cx);
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
                let index = index as usize;
                if let Some(selection) = &mut self.selection {
                    if let Some(on) = selection.get_mut(index) {
                        *on = !*on;
                    }
                    self.revision += 1;
                } else if let Some(entry) = self.entry(index) {
                    let path = files::join(&self.path, &entry.name);
                    if entry.kind != Kind::File {
                        cx.nav().push(Browser::new(self.volume.clone(), self.label, self.root, path, self.shared.clone()));
                    } else if self.shared.pick.is_none() {
                        cx.nav_of::<Root>().push(FilePreviewScreen::new(FileRef { volume: self.volume.clone(), label: self.label, path }));
                    } else if self.openable(entry) {
                        self.finish_file_pick(path, cx);
                    }
                }
            }
            Msg::OpenMenu => {
                if let Some(anchor) = cx.tap_rect() {
                    cx.popup(Popover::new(anchor, self.shared.pick.is_some(), Self::menu, cx.handle()).align(PopoverAlign::End));
                }
            }
            Msg::Refresh | Msg::Reload => self.reload(cx),
            Msg::Select => {
                if matches!(self.entries, Some(Ok(_))) {
                    self.selection = Some(vec![false; self.entries().len()]);
                    self.revision += 1;
                }
            }
            Msg::NewFolder => {
                if matches!(self.entries, Some(Ok(_))) {
                    let taken = self.entries().iter().map(|e| e.name.clone()).collect();
                    let handle = cx.handle();
                    cx.nav_of::<Overlay>().push(NameDialog::new(Naming::NewFolder, self.volume.clone(), self.path.clone(), taken, self.fold_case(), handle));
                }
            }
            Msg::SelectAll => {
                let all = self.all_selected();
                if let Some(selection) = &mut self.selection {
                    selection.iter_mut().for_each(|on| *on = !all);
                    self.revision += 1;
                }
            }
            Msg::Done => self.end_selection(),
            Msg::Move => self.take_to_clipboard(Transfer::Move, cx),
            Msg::Copy => self.take_to_clipboard(Transfer::Copy, cx),
            Msg::Rename => {
                if let [entry] = self.selected().as_slice() {
                    let naming = Naming::Rename { from: entry.name.clone(), folder: entry.kind != Kind::File };
                    let taken = self.entries().iter().filter(|e| e.name != entry.name).map(|e| e.name.clone()).collect();
                    let handle = cx.handle();
                    cx.nav_of::<Overlay>().push(NameDialog::new(naming, self.volume.clone(), self.path.clone(), taken, self.fold_case(), handle));
                }
            }
            Msg::Delete => self.confirm_delete(cx),
            Msg::DeleteConfirmed => self.delete(cx),
            Msg::CancelPaste if self.shared.pick.is_some() => cx.nav_of::<Root>().pop(),
            Msg::CancelPaste => {
                self.shared.clipboard.borrow_mut().take();
                self.clipboard_changed(cx);
            }
            Msg::Paste if self.shared.pick.is_some() => self.start_pick(cx),
            Msg::Paste => self.start_paste(cx),
            Msg::PasteSkip if self.shared.pick.is_some() => cx.nav_of::<Root>().pop(),
            Msg::PasteSkip => self.paste(true, cx),
            Msg::PasteKeepBoth if self.shared.pick.is_some() => self.finish_pick(cx),
            Msg::PasteKeepBoth => self.paste(false, cx),
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let theme = ui.theme();
        let bar = if ui.split_role() == SplitRole::Detail { ui.colors().surface } else { ui.colors().background };
        let clipboard = self.shared.clipboard.borrow();
        let footer = if self.selection.is_some() {
            self.tools(ui)
        } else if let Some(Picking::File(pick)) = self.shared.pick.as_deref() {
            self.file_pick_bar(ui, pick.file_type)
        } else if self.shared.pick.is_some() {
            self.paste_bar(ui, Transfer::Copy, 1, matches!(self.entries, Some(Ok(_))))
        } else if let Some(clip) = clipboard.as_ref() {
            self.paste_bar(ui, clip.transfer, clip.items.len(), self.can_paste(clip))
        } else {
            ui.column([]).height(ui.insets().bottom)
        };
        ui.column([
            ui.column([self.header(ui)]).padding_each(0, ui.insets().top, 0, 0).background(bar),
            ui.text(theme.fonts.caption, self.breadcrumb(ui))
                .foreground(colors::MUTED)
                .ellipsis(Elide::Start)
                .height(48)
                .padding_each(24, 13, 24, 0)
                .border(ui.colors().divider, 1)
                .border_sides(Sides::TOP),
            self.content(ui),
            footer,
        ])
        .background(ui.colors().surface)
    }
}

impl Screen for Browser {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        self.reload(cx);
    }

    fn will_appear(&mut self, cx: &mut Cx<Self>) {
        if self.load.is_none() && self.loaded != CHANGES.load(Ordering::Acquire) {
            self.reload(cx);
        }
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

fn reload(handle: &Handle<Msg>) {
    if !handle.post(Msg::Reload) {
        error!(TAG, "reload: UI queue full");
    }
}
