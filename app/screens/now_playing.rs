/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use core::cell::OnceCell;

use adb::link::Connection;
use agent_link::protocol::{SUBTITLE_ALBUM, SUBTITLE_ARTIST};
use agent_link::{Link, PlayState};
use devkit_gfx::{TextOptions, TextRun};
use devkit_ui::widgets::{slider, slider_value, top_bar};
use devkit_ui::{Align, Canvas, ColorRole, Component, Cx, Drag, Element, Icon, Justify, Part, Prop, Screen, State, Task, This, Ui, id};
use idf::executor;
use idf::time::Instant;

use crate::fonts::{icon_120, icon_36, icon_48, icon_72};
use crate::marquee::{self, Marquee};
use crate::media::{self, Action, DrawSize, Drawn, Progress, Update};
use crate::resources::{self, colors, icons};

const ART: i16 = 552;
const ART_RADIUS: u8 = 24;
const TITLE_PX: u8 = 38;
const TITLE_HEIGHT: i16 = 46;
const PAD: i16 = 24;
const GAP: i16 = 24;
const ROW_GAP: i16 = 16;
const TITLE_GAP: i16 = 12;
const SIDE_WIDTH: i16 = 100;
const SEEK_ROW_HEIGHT: i16 = 72;
const SKIP_BUTTON: i16 = 96;
const PLAY_BUTTON: i16 = 120;
const ICON_BUTTON: i16 = 72;
const PORTRAIT_TITLE_BOTTOM: i16 = 8;
const LANDSCAPE_TITLE_BOTTOM: i16 = 20;
/// Ticks of `marquee::FRAME` between redraws of the position.
const CLOCK_TICKS: u32 = 15;

#[derive(Clone, Copy)]
pub enum Msg {
    Back,
    Control(Action),
    Seek(Drag),
    Volume(Drag),
}

/// The playing track, large, with its controls; needs the agent.
pub struct NowPlayingScreen {
    link: Link,
    conn: Connection,
    progress: Option<Progress>,
    drawn: Option<Drawn>,
    marquee: Marquee,
    ellipsis: OnceCell<TextRun>,
    seeking: Option<u8>,
    volume: u8,
    tasks: Option<(Task, Task)>,
}

impl NowPlayingScreen {
    pub fn new(link: Link, conn: Connection) -> Self {
        Self { link, conn, progress: None, drawn: None, marquee: Marquee::new(), ellipsis: OnceCell::new(), seeking: None, volume: 160, tasks: None }
    }

    fn size() -> DrawSize {
        let fonts = &resources::TOUCH.fonts;
        DrawSize { title_px: TITLE_PX, subtitle_px: fonts.body.px() as u8, art_px: ART as u16, subtitle: SUBTITLE_ARTIST | SUBTITLE_ALBUM }
    }

    fn apply(&mut self, update: Update, cx: &mut Cx<Self>) {
        match update {
            Update::State(progress) => {
                if progress.state.token == 0 {
                    self.drawn = None;
                }
                self.progress = Some(progress);
            }
            Update::Drawn(drawn) => {
                self.drawn = Some(drawn);
                self.marquee.restart();
                cx.redraw(id!("title"));
                cx.redraw(id!("subtitle"));
            }
        }
        cx.rebuild();
    }

    fn seek(&mut self, drag: Drag, cx: &mut Cx<Self>) {
        let Some(progress) = self.progress.as_mut().filter(|p| p.state.can_seek && p.state.duration_ms > 0) else { return };
        let value = slider_value(drag);
        if !drag.ended {
            self.seeking = Some(value);
            return;
        }
        self.seeking = None;
        let position = (u64::from(value) * u64::from(progress.state.duration_ms) / 255) as u32;
        progress.state.position_ms = position;
        progress.at = Instant::now();
        let link = self.link.clone();
        cx.spawn(move |_| async move { media::seek(&link, position).await });
    }

    fn draw_title<'a>(&'a self, canvas: &mut Canvas<'a, '_>) {
        let Some(title) = self.drawn.as_ref().and_then(|d| d.title.as_ref()) else { return };
        let y = (canvas.size().1 - title.height()) / 2;
        let color = canvas.colors().color(ColorRole::Text);
        self.marquee.draw(canvas, title.width(), true, |canvas, x| title.draw_at(canvas, x, y, color));
    }

    fn draw_subtitle<'a>(&'a self, canvas: &mut Canvas<'a, '_>) {
        let Some(subtitle) = self.drawn.as_ref().and_then(|d| d.subtitle.as_ref()) else { return };
        let font = canvas.theme().fonts.body;
        let ellipsis = self.ellipsis.get_or_init(|| {
            let mut run = TextRun::new();
            run.layout(font, "...", &TextOptions::default());
            run
        });
        let color = canvas.colors().color(colors::MUTED);
        subtitle.draw(canvas, font, ellipsis, color, true);
    }

    fn art<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let art = match self.drawn.as_ref().and_then(|d| d.art.as_ref()) {
            Some(art) => ui.image(art),
            None => ui.icon(icon_120(icons::MUSIC)).foreground(colors::MUTED),
        };
        ui.row([art]).size(ART, ART).radius(ART_RADIUS).clip().align(Align::Center).justify(Justify::Center).background(colors::PRESSED).shrink(0)
    }

    fn controls<'a>(&'a self, ui: &'a Ui<Msg>, title_bottom: i16) -> Element<'a, Msg> {
        let theme = ui.theme();
        let state = self.progress.map(|p| p.state);
        let (position, duration) = match state {
            Some(s) if s.duration_ms > 0 => {
                let position = match self.seeking {
                    Some(value) => (u64::from(value) * u64::from(s.duration_ms) / 255) as u32,
                    None => self.progress.map_or(0, |p| p.position_ms()),
                };
                (position, s.duration_ms)
            }
            _ => (0, 0),
        };
        let value = if duration > 0 { (u64::from(position) * 255 / u64::from(duration)) as u8 } else { 0 };
        let time = |ms: u32| ui.text(theme.fonts.body, clock(ui, ms)).text_center().width_fraction(1, 1);
        let seekable = state.is_some_and(|s| s.can_seek && s.duration_ms > 0);
        let seek = row(ui, time(position), player_slider(ui, value, Msg::Seek).disabled(!seekable), time(duration)).height(SEEK_ROW_HEIGHT);
        let playing = state.is_some_and(|s| s.state == PlayState::Playing);
        let button = |glyph: Icon, size: i16, msg: Msg| {
            ui.row([ui.icon(glyph).foreground(ColorRole::Text)]).size(size, size).radius((size / 2) as u8).align(Align::Center).justify(Justify::Center).when(State::PRESSED, &[Prop::Background(colors::PRESSED)]).on_tap(msg)
        };
        let play = if playing { icons::PAUSE_FILLED } else { icons::PLAY_FILLED };
        let buttons = ui
            .row([
                button(icon_48(icons::TRACK_PREVIOUS), SKIP_BUTTON, Msg::Control(Action::Previous)),
                button(icon_72(play), PLAY_BUTTON, Msg::Control(Action::PlayPause)),
                button(icon_48(icons::TRACK_NEXT), SKIP_BUTTON, Msg::Control(Action::Next)),
            ])
            .gap(ROW_GAP)
            .align(Align::Center)
            .justify(Justify::Center);
        let transport = row(ui, ui.empty(), buttons, ui.empty());
        let speaker = ui.row([ui.icon(icon_36(icons::VOLUME_UP)).foreground(ColorRole::Text)]).size(ICON_BUTTON, ICON_BUTTON).align(Align::Center).justify(Justify::Center);
        let volume = row(ui, speaker, player_slider(ui, self.volume, Msg::Volume), ui.empty());
        let fonts = &theme.fonts;
        let title = ui.canvas(Self::draw_title).height(TITLE_HEIGHT).width_fraction(1, 1).id(id!("title"));
        let subtitle = ui.canvas(Self::draw_subtitle).height(fonts.body.line_height()).width_fraction(1, 1).id(id!("subtitle"));
        let titles = ui.column([title, subtitle]).gap(TITLE_GAP).padding_each(0, 0, 0, title_bottom);
        ui.column([titles, seek, transport, volume]).gap(GAP)
    }
}

fn clock<'a, M: Copy>(ui: &'a Ui<M>, ms: u32) -> &'a str {
    let seconds = ms / 1000;
    if seconds >= 3600 {
        ui.format(format_args!("{}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60))
    } else {
        ui.format(format_args!("{}:{:02}", seconds / 60, seconds % 60))
    }
}

/// One line of the controls: what sits on each side keeps `SIDE_WIDTH`, so the
/// middles of every line line up.
fn row<'a>(ui: &'a Ui<Msg>, left: Element<'a, Msg>, middle: Element<'a, Msg>, right: Element<'a, Msg>) -> Element<'a, Msg> {
    let side = |content: Element<'a, Msg>| ui.row([content]).width(SIDE_WIDTH).align(Align::Center).justify(Justify::Center).shrink(0);
    ui.row([side(left), ui.row([middle.grow(1)]).align(Align::Center).grow(1), side(right)]).gap(ROW_GAP).align(Align::Center)
}

fn player_slider<'a>(ui: &'a Ui<Msg>, value: u8, on_drag: fn(Drag) -> Msg) -> Element<'a, Msg> {
    slider(ui, value, on_drag)
        .part(Part::Track, |track| track.background(colors::PLAYER_TRACK))
        .part(Part::Fill, |fill| fill.background(ColorRole::Text))
        .part(Part::Thumb, |thumb| thumb.background(ColorRole::Text))
}

async fn tick(this: This<NowPlayingScreen>) {
    let mut ticks = 0u32;
    loop {
        executor::sleep(marquee::FRAME).await;
        ticks = (ticks + 1) % CLOCK_TICKS;
        let alive = this.with(|s, cx| {
            if s.marquee.moving() {
                cx.redraw(id!("title"));
            }
            if ticks == 0 && s.progress.is_some_and(|p| p.state.speed > 0) {
                cx.rebuild();
            }
        });
        if alive.is_none() {
            return;
        }
    }
}

impl Component for NowPlayingScreen {
    type Msg = Msg;

    fn update(&mut self, msg: Msg, cx: &mut Cx<Self>) {
        match msg {
            Msg::Back => cx.nav().pop(),
            Msg::Control(action) => {
                let (link, conn) = (self.link.clone(), self.conn.clone());
                cx.spawn(move |_| async move { media::control(&link, &conn, action).await });
            }
            Msg::Seek(drag) => self.seek(drag, cx),
            Msg::Volume(drag) => self.volume = slider_value(drag),
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let (width, height) = ui.screen_size();
        let bar = top_bar(ui, Some(Msg::Back), "Now Playing", ui.empty());
        let insets = ui.insets();
        let screen = if width > height {
            let area = ui.column([self.controls(ui, LANDSCAPE_TITLE_BOTTOM)]).justify(Justify::Center).padding_x(PAD).grow(1);
            let left = ui.column([bar, area]).grow(1);
            let right = ui.column([self.art(ui)]).justify(Justify::End).width(ART).padding_each(0, 0, 0, PAD);
            ui.row([left, right]).padding_each(0, insets.top, PAD, insets.bottom)
        } else {
            let art = ui.row([self.art(ui)]).justify(Justify::Center);
            let space = |weight: u8| ui.column([]).grow(weight);
            let contents = ui.column([space(5), art, space(6), self.controls(ui, PORTRAIT_TITLE_BOTTOM), space(8)]).padding_x(PAD).grow(1);
            ui.column([bar, contents]).padding_each(0, insets.top, 0, insets.bottom)
        };
        screen.background(ui.colors().surface)
    }
}

impl Screen for NowPlayingScreen {
    fn will_appear(&mut self, cx: &mut Cx<Self>) {
        let link = self.link.clone();
        let follow = cx.spawn(move |this| media::follow(this, link, Self::size(), Self::apply));
        self.tasks = Some((follow, cx.spawn(tick)));
    }

    fn will_disappear(&mut self, _: &mut Cx<Self>) {
        if let Some((follow, tick)) = self.tasks.take() {
            follow.abort();
            tick.abort();
        }
    }
}
