/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::String;
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::ffi::CStr;

use devkit_gfx::{ImageBuf, ImageFormat, ImageSource};
use devkit_image::{DecodeOptions, Fit, Orientation, ResizeOptions, SliceStream};
use devkit_ui::widgets::progress_indicator;
use devkit_ui::{Align, Cx, Elide, Element, Justify, Overlay, This, Ui, id};
use idf::heap::Caps;
use idf::log::error;

use crate::adb_link;
use crate::apk::{self, ApkInfo, ReadAt};
use crate::file_ops::{self, Endpoint};
use crate::files::FileRef;
use crate::fonts::{icon, small_icon};
use crate::resources::{colors, icons};

use super::file_dialogs::ProgressDialog;
use super::file_preview::{FilePreviewScreen, Msg};
use super::message::MessageScreen;

const TAG: &CStr = c"apk_preview";
const BADGE_SIZE: i16 = 96;
const INSTALL_HEIGHT: i16 = 80;
const GAP: i16 = 24;

#[derive(Clone, Copy)]
pub enum ApkMsg {
    Install,
}

/// A package's name, version and SDK levels, and Install.
pub struct ApkPreview {
    info: Option<Result<ApkInfo, String>>,
    icon: Option<Arc<dyn ImageSource>>,
}

struct Source {
    file: FileRef,
    len: u64,
}

impl ReadAt for Source {
    fn len(&self) -> u64 {
        self.len
    }

    async fn read_at(&mut self, offset: u64, len: usize) -> Result<Vec<u8>, String> {
        self.file.volume.read_at(&self.file.path, offset, len).await
    }
}

fn decode_icon(bytes: &[u8]) -> Option<Arc<dyn ImageSource>> {
    let size = BADGE_SIZE as u16;
    let options = DecodeOptions { format: Some(ImageFormat::Rgb565), target_width: size, target_height: size, ..Default::default() };
    let mut decoder = devkit_image::open(SliceStream::new(bytes), &options).inspect_err(|err| error!(TAG, "icon: {err}")).ok()?;
    let resize = ResizeOptions { target_width: size, target_height: size, fit: Fit::Contain, format: Some(ImageFormat::Rgb565), caps: None };
    let image: ImageBuf<Caps> = devkit_image::resize_decoder(&mut decoder, &resize, Orientation::None).inspect_err(|err| error!(TAG, "icon: {err}")).ok()?;
    Some(Arc::new(image))
}

async fn read(file: FileRef) -> Result<ApkInfo, String> {
    let (len, _) = file.volume.stat(&file.path).await?;
    apk::read(&mut Source { file, len }).await
}

impl ApkPreview {
    pub fn new() -> Self {
        Self { info: None, icon: None }
    }

    pub fn load(file: FileRef, cx: &mut Cx<FilePreviewScreen>) {
        cx.spawn(|this: This<FilePreviewScreen>| async move {
            let info = read(file).await;
            let icon = info.as_ref().ok().and_then(|info| decode_icon(info.icon.as_deref()?));
            this.with(|s, cx| {
                if let Some(apk) = s.apk_mut() {
                    apk.info = Some(info);
                    apk.icon = icon;
                    cx.rebuild();
                }
            });
        });
    }

    pub fn update(screen: &mut FilePreviewScreen, msg: ApkMsg, cx: &mut Cx<FilePreviewScreen>) {
        match msg {
            ApkMsg::Install => {
                let Some(conn) = adb_link::connection() else { return };
                let label = screen.apk_mut().and_then(|apk| apk.info.as_ref()?.as_ref().ok()).map(|info| info.label.clone()).unwrap_or_default();
                let file = screen.file();
                let name = if label.is_empty() { String::from(file.name()) } else { label };
                match file_ops::install(Endpoint::from(&file.volume), conn, file.path.clone()) {
                    Some(op) => cx.nav_of::<Overlay>().push(ProgressDialog::new("Installing", "Install failed", op).on_success("Installed", alloc::format!("{name} was installed."))),
                    None => cx.nav_of::<Overlay>().push(MessageScreen::new("Cannot Install", "Another file operation is running.")),
                }
            }
        }
    }

    pub fn view<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let theme = ui.theme();
        let info = match &self.info {
            None => return ui.column([progress_indicator(ui, None)]).height(BADGE_SIZE * 2).align(Align::Center).justify(Justify::Center),
            Some(Err(err)) => return ui.text(theme.fonts.body, err).foreground(colors::MUTED).wrap(),
            Some(Ok(info)) => info,
        };
        let badge = match &self.icon {
            Some(image) => ui.row([ui.image(image)]).size(BADGE_SIZE, BADGE_SIZE).radius(24).clip().align(Align::Center).justify(Justify::Center),
            None => ui.row([ui.icon(icon(icons::ANDROID)).foreground(colors::ANDROID)]).size(BADGE_SIZE, BADGE_SIZE).radius(24).align(Align::Center).justify(Justify::Center).background(colors::ANDROID_BADGE),
        };
        let name = if info.label.is_empty() { info.package.as_str() } else { info.label.as_str() };
        let title = ui.column([ui.text(theme.fonts.title, name).ellipsis(Elide::End), ui.text(theme.fonts.caption, &info.package).foreground(colors::MUTED).ellipsis(Elide::End)]).gap(6).grow(1);
        let field = |label: &'a str, value: String| ui.column([ui.text(theme.fonts.caption, label).foreground(colors::MUTED), ui.text(theme.fonts.body, ui.store(&value)).ellipsis(Elide::End)]).gap(4).width(0).grow(1);
        let install = ui
            .row([ui.icon(small_icon(icons::DOWNLOAD)), ui.text(theme.fonts.body, "Install")])
            .style(theme.widgets.button.primary)
            .height(INSTALL_HEIGHT)
            .gap(12)
            .align(Align::Center)
            .justify(Justify::Center)
            .on_tap(Msg::Apk(ApkMsg::Install))
            .disabled(adb_link::connection().is_none())
            .id(id!("install"));
        ui.column([ui.row([badge, title]).gap(GAP).align(Align::Center), ui.row([field("Version", info.version()), field("SDK", info.sdk())]).gap(GAP), install]).gap(GAP)
    }
}
