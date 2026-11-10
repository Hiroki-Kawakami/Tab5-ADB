/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::format;
use alloc::string::{String, ToString};
use alloc::sync::Arc;
use alloc::vec;
use alloc::vec::Vec;
use core::ffi::CStr;
use core::time::Duration;

use adb::link::{Connection, State as LinkState};
use agent_link::Link;
use agent_link::protocol::CAP_PREVIEW;
use devkit_gfx::ImageSource;
use devkit_image::Format;
use devkit_ui::widgets::{button_from, button_label, progress_indicator, top_bar};
use devkit_ui::{Align, Canvas, ColorRole, Component, Cx, Element, Justify, Overlay, Prop, Root, Screen, State, StateStyle, Task, This, Ui, id};
use idf::heap::Caps;
use idf::log::{error, info};

use crate::file_ops::{self, Copy, Endpoint};
use crate::fonts::small_icon;
use crate::preview::{self, Picture};
use crate::resources::{colors, icons};
use crate::{adb_link, agent, clock, files};

use super::file_dialogs::ProgressDialog;
use super::message::MessageScreen;

const TAG: &CStr = c"screenshot";
const MARGIN: i16 = 24;
const ACTION_GAP: i16 = 8;
const ACTION_HEIGHT: i16 = 56;
const RADIUS: u8 = 8;
const BUSY: &str = "Another file operation is running.";
const AGENT_QUALITY: u8 = 85;
const AGENT_TIMEOUT: Duration = Duration::from_secs(10);
/// Writing the PNG of a whole screen on the device.
const SAVE_TIMEOUT: Duration = Duration::from_secs(20);

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
    /// A PNG from `screencap`.
    Taken(Arc<Vec<u8, Caps>>),
    /// Kept by the agent, which sends it fitted.
    Kept(Link),
}

/// The screen as taken and its picture, fitted into the stage, which stands
/// in rescaled while the stage's new size is fitted.
pub struct ScreenshotScreen {
    conn: Connection,
    shot: Shot,
    /// The screen's size.
    source: Option<(u16, u16)>,
    area: Option<(u16, u16)>,
    picture: Option<(Arc<Picture>, Arc<dyn ImageSource>)>,
    capturing: Option<Task>,
    fitting: Option<Task>,
    rescaling: Option<Task>,
    saving: bool,
}

impl ScreenshotScreen {
    pub fn new(conn: Connection) -> Self {
        Self { conn, shot: Shot::Capturing, source: None, area: None, picture: None, capturing: None, fitting: None, rescaling: None, saving: false }
    }

    fn show(&mut self, picture: Picture) {
        let picture = Arc::new(picture);
        self.picture = Some((picture.clone(), picture));
    }

    fn abort(task: &mut Option<Task>) {
        if let Some(task) = task.take() {
            task.abort();
        }
    }

    /// Waits for the stage's size, which the agent fits the screen into.
    fn capture(&mut self, cx: &mut Cx<Self>) {
        Self::abort(&mut self.capturing);
        Self::abort(&mut self.fitting);
        Self::abort(&mut self.rescaling);
        let previous = match core::mem::replace(&mut self.shot, Shot::Capturing) {
            Shot::Kept(link) => Some(link),
            _ => None,
        };
        self.picture = None;
        self.source = None;
        let Some(area) = self.area else { return };
        let conn = self.conn.clone();
        self.capturing = Some(cx.spawn(move |this: This<Self>| async move {
            if let Some(link) = agent::link_with(CAP_PREVIEW) {
                match take_on_agent(&link, area).await {
                    Ok((picture, source)) => {
                        this.with(|s, cx| {
                            s.capturing = None;
                            s.shot = Shot::Kept(link);
                            s.source = Some(source);
                            s.show(picture);
                            if s.area != Some(area) {
                                s.fit(cx);
                            }
                            cx.rebuild();
                        });
                        return;
                    }
                    Err(err) => error!(TAG, "agent capture: {err}"),
                }
            }
            if let Some(previous) = previous {
                let _ = previous.screenshot_release(AGENT_TIMEOUT).await;
            }
            let result = match adb_link::spawner() {
                Some(spawner) => preview::capture(spawner, &conn, false).await,
                None => Err(String::from("adb not started")),
            };
            this.with(|s, cx| {
                s.capturing = None;
                s.captured(result, cx);
                cx.rebuild();
            });
        }));
    }

    fn captured(&mut self, result: Result<Vec<u8, Caps>, String>, cx: &mut Cx<Self>) {
        let shot = result.and_then(|shot| match devkit_image::sniff(&shot) {
            Some(Format::Png) => Ok(shot),
            _ => Err(String::from("not a PNG")),
        });
        match shot {
            Ok(shot) => {
                info!(TAG, "captured {} bytes", shot.len());
                self.source = png_size(&shot);
                self.shot = Shot::Taken(Arc::new(shot));
                self.fit(cx);
            }
            Err(err) => {
                error!(TAG, "capture: {err}");
                self.shot = Shot::Failed(if matches!(self.conn.state(), LinkState::Closed(_)) { "Not connected." } else { "Capture failed." });
            }
        }
    }

    fn fit(&mut self, cx: &mut Cx<Self>) {
        Self::abort(&mut self.fitting);
        let Some(area) = self.area else { return };
        let fitting = match &self.shot {
            Shot::Taken(shot) => {
                let shot = shot.clone();
                cx.spawn(move |this: This<Self>| async move {
                    let result = preview::fit_on_worker(shot, area).await;
                    this.with(|s, cx| s.fitted(result, cx));
                })
            }
            Shot::Kept(link) => {
                let link = link.clone();
                cx.spawn(move |this: This<Self>| async move {
                    let result = render_on_agent(&link, area).await;
                    this.with(|s, cx| s.fitted(result, cx));
                })
            }
            _ => return,
        };
        self.fitting = Some(fitting);
    }

    fn fitted(&mut self, result: Result<Picture, String>, cx: &mut Cx<Self>) {
        self.fitting = None;
        Self::abort(&mut self.rescaling);
        match result {
            Ok(picture) => self.show(picture),
            Err(err) => {
                error!(TAG, "fit: {err}");
                self.picture = None;
                self.shot = Shot::Failed("Capture failed.");
            }
        }
        cx.rebuild();
    }

    fn stand_in(&mut self, area: (u16, u16), cx: &mut Cx<Self>) {
        Self::abort(&mut self.rescaling);
        let (Some((picture, _)), Some(source)) = (&self.picture, self.source) else {
            self.picture = None;
            return;
        };
        let picture = picture.clone();
        let size = preview::fit_size(source, area);
        self.rescaling = Some(cx.spawn(move |this: This<Self>| async move {
            let result = preview::rescale(&picture, size).await;
            drop(picture);
            this.with(|s, cx| {
                s.rescaling = None;
                match result {
                    Ok(stand_in) if s.fitting.is_some() && s.area == Some(area) => s.show(stand_in),
                    Ok(_) => {}
                    Err(err) => error!(TAG, "rescale: {err}"),
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
        match self.shot {
            Shot::Capturing if self.capturing.is_none() => self.capture(cx),
            Shot::Taken(_) | Shot::Kept(_) => {
                self.fit(cx);
                self.stand_in(area, cx);
            }
            _ => {}
        }
    }

    fn save(&mut self, cx: &mut Cx<Self>) {
        let failed = |cx: &mut Cx<Self>, text: &str| cx.nav_of::<Overlay>().push(MessageScreen::new("Cannot Save", text.to_string()));
        if !matches!(self.shot, Shot::Taken(_) | Shot::Kept(_)) {
            return;
        }
        if !files::sd_present() {
            return failed(cx, "No SD card slot.");
        }
        if let Err(err) = files::mount_sd() {
            return failed(cx, &err);
        }
        let base = clock::file_stamp().map_or(String::from("screenshot.png"), |stamp| format!("screenshot_{stamp}.png"));
        let taken = |name: &str| files::c_path(&files::join(files::SD_ROOT, name)).is_ok_and(|path| idf::fs::metadata(&path).is_ok());
        let name = files::unused_name(&base, false, taken);
        match &self.shot {
            Shot::Taken(shot) => {
                let mut data = match idf::heap::try_vec(shot.len(), Caps::SPIRAM) {
                    Ok(data) => data,
                    Err(err) => {
                        error!(TAG, "save buffer: {err}");
                        return failed(cx, "Not enough memory to save the screenshot.");
                    }
                };
                data.extend_from_slice(shot);
                match file_ops::save(data, files::join(files::SD_ROOT, &name)) {
                    Some(op) => cx.nav_of::<Overlay>().push(ProgressDialog::new("Saving", "Save failed", op).on_success("Saved", format!("Saved as {name} on the SD card."))),
                    None => failed(cx, BUSY),
                }
            }
            Shot::Kept(link) => {
                let link = link.clone();
                self.saving = true;
                cx.spawn(move |this: This<Self>| async move {
                    let saved = link.screenshot_save(SAVE_TIMEOUT).await;
                    this.with(|s, cx| {
                        s.saving = false;
                        cx.rebuild();
                        let (_, path) = match saved {
                            Ok(saved) => saved,
                            Err(err) => {
                                error!(TAG, "agent save: {err}");
                                return failed(cx, "The screenshot could not be saved.");
                            }
                        };
                        let copies = vec![Copy { source: path, target: files::join(files::SD_ROOT, &name) }];
                        match file_ops::copy(Endpoint::Android(s.conn.clone()), Endpoint::Sd, copies) {
                            Some(op) => cx.nav_of::<Overlay>().push(ProgressDialog::new("Saving", "Save failed", op).on_success("Saved", format!("Saved as {name} on the SD card."))),
                            None => failed(cx, BUSY),
                        }
                    });
                });
            }
            _ => {}
        }
    }

    fn draw_nothing<'a>(&'a self, _: &mut Canvas<'a, '_>) {}

    fn stage<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let probe = ui.canvas(Self::draw_nothing).absolute(Some(0), Some(0), Some(0), Some(0)).on_size(Msg::Area);
        let content = match (&self.shot, &self.picture) {
            (Shot::Taken(_) | Shot::Kept(_), Some((_, picture))) => ui.column([ui.image(picture)]).radius(RADIUS).clip(),
            (Shot::Failed(text), _) => ui.text(ui.theme().fonts.body, text).foreground(colors::MUTED),
            _ => progress_indicator(ui, None),
        };
        ui.column([probe, content]).grow(1).align_self(Align::Stretch).align(Align::Center).justify(Justify::Center).clip()
    }
}

async fn take_on_agent(link: &Link, (width, height): (u16, u16)) -> Result<(Picture, (u16, u16)), String> {
    let reply = link.screenshot(width, height, AGENT_QUALITY, AGENT_TIMEOUT).await.map_err(|err| format!("{err}"))?;
    let rendered = reply.rendered();
    let size = |key| match rendered.attr(key) {
        Some(agent_link::AttrValue::U32(v)) => Some(v.min(u32::from(u16::MAX)) as u16),
        _ => None,
    };
    let source = size(agent_link::protocol::ATTR_WIDTH).zip(size(agent_link::protocol::ATTR_HEIGHT)).unwrap_or((rendered.width, rendered.height));
    Ok((preview::decode_jpeg(rendered.jpeg).await?, source))
}

async fn render_on_agent(link: &Link, (width, height): (u16, u16)) -> Result<Picture, String> {
    let reply = link.screenshot_render(width, height, AGENT_QUALITY, AGENT_TIMEOUT).await.map_err(|err| format!("{err}"))?;
    preview::decode_jpeg(reply.rendered().jpeg).await
}

/// From the IHDR, which every PNG starts with.
fn png_size(png: &[u8]) -> Option<(u16, u16)> {
    let word = |at: usize| png.get(at..at + 4).map(|b| u32::from_be_bytes([b[0], b[1], b[2], b[3]]));
    Some((word(16)?.min(u32::from(u16::MAX)) as u16, word(20)?.min(u32::from(u16::MAX)) as u16))
}

impl Drop for ScreenshotScreen {
    fn drop(&mut self) {
        let link = match &self.shot {
            Shot::Kept(link) => Some(link.clone()),
            _ => agent::link_with(CAP_PREVIEW),
        };
        if let (Some(link), Some(spawner)) = (link, adb_link::spawner()) {
            drop(spawner.spawn(async move {
                if let Err(err) = link.screenshot_release(AGENT_TIMEOUT).await {
                    error!(TAG, "agent release: {err}");
                }
            }));
        }
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
        let taken = matches!(self.shot, Shot::Taken(_) | Shot::Kept(_)) && !self.saving;
        let actions = ui
            .row([
                button_from(ui.row([ui.icon(small_icon(icons::REFRESH)), button_label(ui, "Re-capture")]), &TONAL, Msg::Capture).height(ACTION_HEIGHT).disabled(capturing || self.saving).id(id!("capture")),
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
