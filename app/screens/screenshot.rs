/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::{String, ToString};
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::ffi::CStr;

use adb::link::{Connection, State as LinkState};
use devkit_gfx::ImageSource;
use devkit_image::Format;
use devkit_ui::widgets::{button_from, button_label, progress_indicator, top_bar};
use devkit_ui::{Align, Canvas, ColorRole, Component, Cx, Element, Justify, Overlay, Prop, Root, Screen, State, StateStyle, Task, Ui, id};
use idf::heap::Caps;
use idf::log::{error, info};

use crate::fonts::small_icon;
use crate::preview;
use crate::resources::{colors, icons};
use crate::{adb_link, clock, file_ops, files};

use super::file_dialogs::ProgressDialog;
use super::message::MessageScreen;

const TAG: &CStr = c"screenshot";
const MARGIN: i16 = 24;
const ACTION_GAP: i16 = 8;
const ACTION_HEIGHT: i16 = 56;
const RADIUS: u8 = 8;
const BUSY: &str = "Another file operation is running.";

static TONAL: StateStyle = StateStyle {
    font: None,
    base: &[Prop::Background(colors::PRESSED), Prop::Foreground(ColorRole::Text), Prop::Radius(14), Prop::PaddingX(24), Prop::Gap(12)],
    states: &[(State::PRESSED, &[Prop::Background(ColorRole::Divider)]), (State::DISABLED, &[Prop::Opacity(102)])],
};

#[derive(Clone, Copy)]
pub enum Msg {
    Back,
    Capture,
    Save,
    Area(i16, i16),
}

enum Shot {
    Capturing,
    Failed(&'static str),
    Taken(Arc<Vec<u8, Caps>>),
}

pub struct ScreenshotScreen {
    conn: Connection,
    shot: Shot,
    area: Option<(u16, u16)>,
    picture: Option<Arc<dyn ImageSource>>,
    fitting: Option<Task>,
}

impl ScreenshotScreen {
    pub fn new(conn: Connection) -> Self {
        Self { conn, shot: Shot::Capturing, area: None, picture: None, fitting: None }
    }

    fn capture(&mut self, cx: &mut Cx<Self>) {
        self.stop_fitting();
        self.shot = Shot::Capturing;
        self.picture = None;
        let conn = self.conn.clone();
        cx.spawn(|this| async move {
            let result = match adb_link::spawner() {
                Some(spawner) => preview::capture(spawner, &conn, false).await,
                None => Err(String::from("adb not started")),
            };
            this.with(|s, cx| {
                s.captured(result, cx);
                cx.rebuild();
            });
        });
    }

    fn captured(&mut self, result: Result<Vec<u8, Caps>, String>, cx: &mut Cx<Self>) {
        let shot = result.and_then(|shot| match devkit_image::sniff(&shot) {
            Some(Format::Png) => Ok(shot),
            _ => Err(String::from("not a PNG")),
        });
        match shot {
            Ok(shot) => {
                info!(TAG, "captured {} bytes", shot.len());
                self.shot = Shot::Taken(Arc::new(shot));
                self.fit(cx);
            }
            Err(err) => {
                error!(TAG, "capture: {err}");
                self.shot = Shot::Failed(if matches!(self.conn.state(), LinkState::Closed(_)) { "Not connected." } else { "Capture failed." });
            }
        }
    }

    fn stop_fitting(&mut self) {
        if let Some(task) = self.fitting.take() {
            task.abort();
        }
    }

    fn fit(&mut self, cx: &mut Cx<Self>) {
        self.stop_fitting();
        let (Shot::Taken(shot), Some(area)) = (&self.shot, self.area) else { return };
        let shot = shot.clone();
        self.fitting = Some(cx.spawn(move |this| async move {
            let result = preview::fit_on_worker(shot, area).await;
            this.with(|s, cx| {
                match result {
                    Ok(picture) => s.picture = Some(Arc::new(picture)),
                    Err(err) => {
                        error!(TAG, "fit: {err}");
                        s.shot = Shot::Failed("Capture failed.");
                    }
                }
                cx.rebuild();
            });
        }));
    }

    fn resized(&mut self, width: i16, height: i16, cx: &mut Cx<Self>) {
        let area = (width.max(1) as u16, height.max(1) as u16);
        if self.area == Some(area) {
            return;
        }
        self.area = Some(area);
        self.picture = None;
        self.fit(cx);
    }

    fn save(&mut self, cx: &mut Cx<Self>) {
        let failed = |cx: &mut Cx<Self>, text: &str| cx.nav_of::<Overlay>().push(MessageScreen::new("Cannot Save", text.to_string()));
        let Shot::Taken(shot) = &self.shot else { return };
        if !files::sd_present() {
            return failed(cx, "No SD card slot.");
        }
        if let Err(err) = files::mount_sd() {
            return failed(cx, &err);
        }
        let base = clock::file_stamp().map_or(String::from("screenshot.png"), |stamp| alloc::format!("screenshot_{stamp}.png"));
        let taken = |name: &str| files::c_path(&files::join(files::SD_ROOT, name)).is_ok_and(|path| idf::fs::metadata(&path).is_ok());
        let name = files::unused_name(&base, false, taken);
        let mut data = match idf::heap::try_vec(shot.len(), Caps::SPIRAM) {
            Ok(data) => data,
            Err(err) => {
                error!(TAG, "save buffer: {err}");
                return failed(cx, "Not enough memory to save the screenshot.");
            }
        };
        data.extend_from_slice(shot);
        match file_ops::save(data, files::join(files::SD_ROOT, &name)) {
            Some(op) => cx.nav_of::<Overlay>().push(ProgressDialog::new("Saving", "Save failed", op).on_success("Saved", alloc::format!("Saved as {name} on the SD card."))),
            None => failed(cx, BUSY),
        }
    }

    fn draw_nothing<'a>(&'a self, _: &mut Canvas<'a, '_>) {}

    fn stage<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let probe = ui.canvas(Self::draw_nothing).absolute(Some(0), Some(0), Some(0), Some(0)).on_size(Msg::Area);
        let content = match (&self.shot, &self.picture) {
            (Shot::Taken(_), Some(picture)) => ui.column([ui.image(picture)]).radius(RADIUS).clip(),
            (Shot::Failed(text), _) => ui.text(ui.theme().fonts.body, text).foreground(colors::MUTED),
            _ => progress_indicator(ui, None),
        };
        ui.column([probe, content]).grow(1).align_self(Align::Stretch).align(Align::Center).justify(Justify::Center)
    }
}

impl Component for ScreenshotScreen {
    type Msg = Msg;

    fn update(&mut self, msg: Msg, cx: &mut Cx<Self>) {
        match msg {
            Msg::Back => cx.nav_of::<Root>().pop(),
            Msg::Capture => self.capture(cx),
            Msg::Save => self.save(cx),
            Msg::Area(width, height) => self.resized(width, height, cx),
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let b = &ui.theme().widgets.button;
        let capturing = matches!(self.shot, Shot::Capturing);
        let taken = matches!(self.shot, Shot::Taken(_));
        let actions = ui
            .row([
                button_from(ui.row([ui.icon(small_icon(icons::REFRESH)), button_label(ui, "Re-capture")]), &TONAL, Msg::Capture).height(ACTION_HEIGHT).disabled(capturing).id(id!("capture")),
                button_from(ui.row([ui.icon(small_icon(icons::DOWNLOAD)), button_label(ui, "Save")]), b.primary, Msg::Save).height(ACTION_HEIGHT).disabled(!taken).id(id!("save")),
            ])
            .gap(ACTION_GAP)
            .padding_each(0, 0, MARGIN - ui.theme().widgets.top_bar.padding_x, 0);
        ui.column([top_bar(ui, Some(Msg::Back), "Screenshot", actions), ui.column([self.stage(ui)]).grow(1).padding_each(MARGIN, 4, MARGIN, MARGIN + ui.insets().bottom)])
            .padding_each(0, ui.insets().top, 0, 0)
            .background(ui.colors().background)
    }
}

impl Screen for ScreenshotScreen {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        self.capture(cx);
    }
}
