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

use devkit_gfx::FontChain;
use devkit_ui::widgets::{divider, icon_button, menu, menu_item, top_bar};
use devkit_ui::{Align, Component, Cx, Elide, Element, Overlay, Popover, PopoverAlign, Root, Screen, Ui, id};
use idf::log::error;
use idf::sync::Mutex;

use crate::adb_link;
use crate::file_ops::{self, Copy, Endpoint, Operation};
use crate::files::{self, FileRef, FileType, Volume};
use crate::fonts::small_icon;
use crate::resources::{colors, icons};

use super::apk_preview::{ApkMsg, ApkPreview};
use super::file_dialogs::{Action, Choice, ProgressDialog};
use super::file_manager::{FileManagerScreen, FolderPick};
use super::message::MessageScreen;

const TAG: &CStr = c"file_preview";
const MARGIN: i16 = 24;
const GAP: i16 = 20;
const RADIUS: u8 = 24;
const INFO_WIDTH_LANDSCAPE: i16 = 440;
const INFO_ROW_HEIGHT: i16 = 64;
const BUSY: &str = "Another file operation is running.";

/// What shows a file's content above its details; a type without one shows
/// only the details.
pub enum Preview {
    Apk(ApkPreview),
}

impl Preview {
    fn of(file: &FileRef) -> Option<Self> {
        match files::file_type(file.name()) {
            FileType::Apk => Some(Preview::Apk(ApkPreview::new())),
            _ => None,
        }
    }
}

#[derive(Clone, Copy)]
pub enum Msg {
    Back,
    OpenMenu,
    Copy,
    Picked,
    Delete,
    DeleteConfirmed,
    Deleted,
    Apk(ApkMsg),
}

pub struct FilePreviewScreen {
    file: FileRef,
    stat: Option<Result<(u64, i64), String>>,
    preview: Option<Preview>,
    picked: Option<Arc<Mutex<Option<(Volume, String)>>>>,
    deleting: Option<Operation>,
}

impl FilePreviewScreen {
    pub fn new(file: FileRef) -> Self {
        let preview = Preview::of(&file);
        Self { file, stat: None, preview, picked: None, deleting: None }
    }

    pub fn file(&self) -> &FileRef {
        &self.file
    }

    pub fn apk_mut(&mut self) -> Option<&mut ApkPreview> {
        match &mut self.preview {
            Some(Preview::Apk(apk)) => Some(apk),
            None => None,
        }
    }

    fn pick_destination(&mut self, cx: &mut Cx<Self>) {
        let result = Arc::new(Mutex::new(None));
        let handle = cx.handle();
        let done = Box::new(move || {
            if !handle.post(Msg::Picked) {
                error!(TAG, "picked: UI queue full");
            }
        });
        let pick = FolderPick { name: self.file.name().to_string(), result: result.clone(), done };
        self.picked = Some(result);
        cx.nav_of::<Root>().push(FileManagerScreen::pick_folder(adb_link::connection().is_some(), pick));
    }

    fn copy_to_picked(&mut self, cx: &mut Cx<Self>) {
        let Some((volume, target)) = self.picked.take().and_then(|picked| picked.lock().take()) else { return };
        let copies = vec![Copy { source: self.file.path.clone(), target }];
        match file_ops::copy(Endpoint::from(&self.file.volume), Endpoint::from(&volume), copies) {
            Some(op) => cx.nav_of::<Overlay>().push(ProgressDialog::new("Copying", "Copy failed", op)),
            None => cx.nav_of::<Overlay>().push(MessageScreen::new("Cannot Copy", BUSY)),
        }
    }

    fn menu<'a>(_: &'a (), ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        menu(ui, [
            menu_item(ui, Some(small_icon(icons::COPY)), "Copy", Msg::Copy).id(id!("menu-copy")),
            menu_item(ui, Some(small_icon(icons::TRASH)), "Delete", Msg::Delete).foreground(colors::DESTRUCTIVE).id(id!("menu-delete")),
        ])
    }

    fn delete(&mut self, cx: &mut Cx<Self>) {
        let Some(op) = file_ops::delete(Endpoint::from(&self.file.volume), vec![self.file.path.clone()]) else {
            cx.nav_of::<Overlay>().push(MessageScreen::new("Cannot Delete", BUSY));
            return;
        };
        self.deleting = Some(op.clone());
        let handle = cx.handle();
        let done = move || {
            if !handle.post(Msg::Deleted) {
                error!(TAG, "deleted: UI queue full");
            }
        };
        cx.nav_of::<Overlay>().push(ProgressDialog::new("Deleting", "Delete failed", op).on_done(done));
    }

    fn location<'a>(&'a self, ui: &'a Ui<Msg>) -> &'a str {
        let path = match self.file.volume {
            Volume::Sd => self.file.path.strip_prefix(files::SD_ROOT).unwrap_or(&self.file.path),
            Volume::Android(_) => &self.file.path,
        };
        ui.format(format_args!("{} \u{203a} {path}", self.file.label))
    }

    fn info<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let (size, modified) = match &self.stat {
            Some(Ok((size, mtime))) => (ui.store(&files::format_bytes(*size)), ui.store(&files::format_date_time(*mtime))),
            _ => ("", ""),
        };
        let rows: Vec<Element<'a, Msg>> = vec![
            stacked(ui, "Name", self.file.name(), ui.theme().fonts.body),
            divider(ui),
            inline(ui, "Type", files::type_label(files::file_type(self.file.name()))),
            divider(ui),
            inline(ui, "Size", size),
            divider(ui),
            inline(ui, "Modified", modified),
            divider(ui),
            stacked(ui, "Location", self.location(ui), ui.theme().fonts.caption),
        ];
        card(ui, rows).padding_y(4)
    }
}

fn card<'a>(ui: &'a Ui<Msg>, children: Vec<Element<'a, Msg>>) -> Element<'a, Msg> {
    ui.column(children).padding_x(MARGIN).radius(RADIUS).background(ui.colors().surface)
}

fn stacked<'a>(ui: &'a Ui<Msg>, label: &'a str, value: &'a str, font: &'static FontChain) -> Element<'a, Msg> {
    ui.column([ui.text(ui.theme().fonts.caption, label).foreground(colors::MUTED), ui.text(font, value).ellipsis(Elide::Middle)]).gap(4).padding_y(16)
}

fn inline<'a>(ui: &'a Ui<Msg>, label: &'a str, value: &'a str) -> Element<'a, Msg> {
    let theme = ui.theme();
    ui.row([ui.text(theme.fonts.caption, label).foreground(colors::MUTED).grow(1), ui.text(theme.fonts.body, value)]).height(INFO_ROW_HEIGHT).align(Align::Center)
}

async fn load_stat(this: devkit_ui::This<FilePreviewScreen>, file: FileRef) {
    let stat = file.volume.stat(&file.path).await;
    this.with(|s, cx| {
        s.stat = Some(stat);
        cx.rebuild();
    });
}

impl Component for FilePreviewScreen {
    type Msg = Msg;

    fn update(&mut self, msg: Msg, cx: &mut Cx<Self>) {
        match msg {
            Msg::Back => cx.nav_of::<Root>().pop(),
            Msg::OpenMenu => {
                if let Some(anchor) = cx.tap_rect() {
                    cx.popup(Popover::new(anchor, (), Self::menu, cx.handle()).align(PopoverAlign::End));
                }
            }
            Msg::Copy => self.pick_destination(cx),
            Msg::Picked => self.copy_to_picked(cx),
            Msg::Delete => {
                let message = alloc::format!("Are you sure you want to delete \"{}\"?\nThis can't be undone.", self.file.name());
                let buttons = vec![("Cancel", Action::Secondary, None), ("Delete", Action::Destructive, Some(Msg::DeleteConfirmed))];
                let handle = cx.handle();
                cx.nav_of::<Overlay>().push(Choice::new("Delete Permanently", message, buttons, handle));
            }
            Msg::DeleteConfirmed => self.delete(cx),
            Msg::Deleted => {
                if self.deleting.take().is_some_and(|op| {
                    let progress = op.progress();
                    progress.error.is_none() && !progress.cancelled
                }) {
                    cx.nav_of::<Root>().pop();
                }
            }
            Msg::Apk(msg) => ApkPreview::update(self, msg, cx),
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let theme = ui.theme();
        let more = icon_button(ui, theme.icons.more, Msg::OpenMenu).id(id!("more"));
        let (width, height) = ui.screen_size();
        let preview = match &self.preview {
            Some(Preview::Apk(apk)) => Some(apk.view(ui).padding(MARGIN).radius(RADIUS).background(ui.colors().surface)),
            None => None,
        };
        let body = match preview {
            Some(preview) if width > height => ui.row([preview.width(0).grow(1), self.info(ui).width(INFO_WIDTH_LANDSCAPE)]).gap(GAP).align(Align::Start),
            Some(preview) => ui.column([preview, self.info(ui)]).gap(GAP),
            None => ui.column([self.info(ui)]),
        };
        ui.column([top_bar(ui, Some(Msg::Back), self.file.name(), more), ui.scroll(body.padding_each(MARGIN, 4, MARGIN, MARGIN + ui.insets().bottom)).grow(1)])
            .padding_each(0, ui.insets().top, 0, 0)
            .background(ui.colors().background)
    }
}

impl Screen for FilePreviewScreen {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        let file = self.file.clone();
        cx.spawn(|this| load_stat(this, file));
        if let Some(Preview::Apk(_)) = &self.preview {
            ApkPreview::load(self.file.clone(), cx);
        }
    }
}
