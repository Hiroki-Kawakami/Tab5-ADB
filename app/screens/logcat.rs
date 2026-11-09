/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::rc::Rc;
use alloc::string::{String, ToString};
use alloc::vec;
use alloc::vec::Vec;
use core::cell::RefCell;
use core::ffi::CStr;
use core::future::{Future, poll_fn};
use core::pin::pin;
use core::task::Poll;
use core::time::Duration;

use adb::link::Connection;
use devkit_ui::widgets::{icon_button, segmented, text_field, top_bar};
use devkit_ui::{Align, CollectionLayout, ColorRole, Component, Corners, Cx, Edge, Elide, Element, Handle, Justify, Overlay, Part, Presence, Prop, Root, Screen, ScrollEdge, ScrollPosition, ScrollTarget, Shadow, Sides, State, StateStyle, Task, TextBuffer, TextInput, This, Ui, id};
use idf::executor;
use idf::heap::Caps;
use idf::log::error;

use crate::fonts::{MONO, icon, small_icon};
use crate::logcat::{self, Filter, Level, LineSplitter, Log};
use crate::resources::{colors, icons};
use crate::{clock, file_ops, files};

use super::file_dialogs::ProgressDialog;
use super::message::MessageScreen;

const TAG: &CStr = c"logcat";
const ROW_HEIGHT: i16 = 60;
const ROW_PADDING: i16 = 16;
const MARGIN: i16 = 24;
const GAP: i16 = 16;
const LEVEL_WIDTH: i16 = 56;
const TAB_HEIGHT: i16 = 56;
const JUMP_SIZE: i16 = 64;
const SHEET_WIDTH: i16 = 880;
const SHEET_RADIUS: u8 = 24;
const ACTION_HEIGHT: i16 = 72;
const MESSAGE_MAX_HEIGHT: i16 = 240;
const TIME_WIDTH: i16 = 296;
const ID_WIDTH: i16 = 156;
const READ_CHUNK: usize = 16 * 1024;
const PENDING_MAX: usize = 256 * 1024;
const FLUSH: Duration = Duration::from_millis(100);
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
    states: &[(State::PRESSED, &[Prop::Background(colors::PRESSED)]), (State::DISABLED, &[Prop::Opacity(102)])],
};

fn level_color(level: Level) -> ColorRole {
    match level {
        Level::Verbose => colors::MUTED,
        Level::Debug => colors::LOG_DEBUG,
        Level::Info => colors::LOG_INFO,
        Level::Warning => colors::LOG_WARNING,
        Level::Error | Level::Fatal => colors::LOG_ERROR,
    }
}

fn message_color(level: Level) -> ColorRole {
    match level {
        Level::Verbose => colors::MUTED,
        Level::Debug | Level::Info => ColorRole::Text,
        Level::Warning => colors::LOG_WARNING,
        Level::Error | Level::Fatal => colors::LOG_ERROR,
    }
}

fn tonal_color(level: Level) -> ColorRole {
    match level {
        Level::Verbose => colors::PRESSED,
        Level::Debug => colors::LOG_DEBUG_TONAL,
        Level::Info => colors::LOG_INFO_TONAL,
        Level::Warning => colors::LOG_WARNING_TONAL,
        Level::Error | Level::Fatal => colors::LOG_ERROR_TONAL,
    }
}

#[derive(Clone, Copy)]
pub enum Msg {
    Back,
    Level(u8),
    Input(TextInput),
    Pause,
    Clear,
    Save,
    Open(u32),
    Scrolled(ScrollPosition),
    Latest,
    /// From a line's details, by sequence number: show the next newer one or
    /// the next older one.
    Step(u32, bool),
    FilterTag(u32),
    FilterPid(u32),
}

pub struct LogcatScreen {
    conn: Connection,
    log: Log<Caps>,
    splitter: LineSplitter,
    filter: TextBuffer,
    session: Option<Task>,
    revision: u32,
    offset: i32,
    at_end: bool,
    detail: Rc<RefCell<DetailLink>>,
}

impl LogcatScreen {
    pub fn new(conn: Connection) -> Self {
        Self {
            conn,
            log: Log::new_in(Caps::SPIRAM),
            splitter: LineSplitter::new(),
            filter: TextBuffer::new(),
            session: None,
            revision: 0,
            offset: 0,
            at_end: true,
            detail: Rc::new(RefCell::new(DetailLink { sheet: None, next: None })),
        }
    }

    fn start(&mut self, cx: &mut Cx<Self>) {
        let command = match self.log.last_time() {
            Some(time) => alloc::format!("logcat -v threadtime -T '{time}'"),
            None => String::from(logcat::START_COMMAND),
        };
        self.log.resume();
        self.splitter = LineSplitter::new();
        let conn = self.conn.clone();
        self.session = Some(cx.spawn(|this| session(this, conn, command)));
    }

    fn receive(&mut self, data: &[u8], dropped: bool, cx: &mut Cx<Self>) {
        let Self { log, splitter, .. } = self;
        let mut removed = 0;
        splitter.feed(data, |line| removed += log.push(line));
        if dropped {
            removed += self.log.push_notice("[output dropped]");
        }
        self.removed(removed, cx);
    }

    fn ended(&mut self, notice: &str, cx: &mut Cx<Self>) {
        self.session = None;
        let removed = self.log.push_notice(notice);
        self.removed(removed, cx);
    }

    /// Keeps the lines in view where they are when older ones went.
    fn removed(&mut self, count: usize, cx: &mut Cx<Self>) {
        if count == 0 {
            return;
        }
        self.revision += 1;
        if !self.at_end {
            self.offset = (self.offset - count as i32 * i32::from(ROW_HEIGHT)).max(0);
            cx.scroll_to(id!("lines"), ScrollTarget::Offset(self.offset), false);
        }
    }


    fn refiltered(&mut self, cx: &mut Cx<Self>) {
        self.revision += 1;
        self.at_end = true;
        cx.scroll_to(id!("lines"), ScrollTarget::End, false);
    }

    fn set_filter_text(&mut self, text: &str, cx: &mut Cx<Self>) {
        self.filter.set_text(text);
        self.log.set_filter(Filter::parse(text));
        self.refiltered(cx);
        cx.nav_of::<Overlay>().pop();
    }

    fn detail_line(&self, seq: u32) -> Option<DetailLine> {
        let index = self.log.shown_index(seq)?;
        let line = self.log.get(seq)?;
        Some(DetailLine {
            seq,
            level: line.level,
            header: line.header().map(|h| (h.time.to_string(), h.pid, h.tid, h.tag.to_string())),
            message: line.message().to_string(),
            older: index > 0,
            newer: index + 1 < self.log.shown_len(),
        })
    }

    fn open(&mut self, seq: u32, cx: &mut Cx<Self>) {
        let Some(line) = self.detail_line(seq) else { return };
        let detail = LogDetail::new(self.conn.clone(), line, cx.handle(), self.detail.clone());
        cx.nav_of::<Overlay>().push(detail);
    }

    /// Shows the next line in the sheet already open.
    fn step(&mut self, seq: u32, newer: bool, cx: &mut Cx<Self>) {
        let Some(index) = self.log.shown_index(seq) else { return };
        let next = if newer { index + 1 } else { index.wrapping_sub(1) };
        let Some(line) = self.log.shown_seq(next).and_then(|next_seq| self.detail_line(next_seq)) else { return };
        cx.scroll_to(id!("lines"), ScrollTarget::Index(next), true);
        let mut link = self.detail.borrow_mut();
        let Some(sheet) = link.sheet.clone() else { return };
        link.next = Some(line);
        if !sheet.post(DetailMsg::Load) {
            error!(TAG, "step: UI queue full");
        }
    }

    fn save(&mut self, cx: &mut Cx<Self>) {
        let failed = |cx: &mut Cx<Self>, text: &str| cx.nav_of::<Overlay>().push(MessageScreen::new("Cannot Save", text.to_string()));
        if !files::sd_present() {
            return failed(cx, "No SD card slot.");
        }
        if let Err(err) = files::mount_sd() {
            return failed(cx, &err);
        }
        let base = clock::file_stamp().map_or(String::from("logcat.txt"), |stamp| alloc::format!("logcat_{stamp}.txt"));
        let taken = |name: &str| files::c_path(&files::join(files::SD_ROOT, name)).is_ok_and(|path| idf::fs::metadata(&path).is_ok());
        let name = files::unused_name(&base, false, taken);
        let mut data = match idf::heap::try_vec(self.log.text_bytes(), Caps::SPIRAM) {
            Ok(data) => data,
            Err(err) => {
                error!(TAG, "save buffer: {err}");
                return failed(cx, "Not enough memory to save the log.");
            }
        };
        self.log.write_to(&mut data);
        match file_ops::save(data, files::join(files::SD_ROOT, &name)) {
            Some(op) => cx.nav_of::<Overlay>().push(ProgressDialog::new("Saving", "Save failed", op).on_success("Saved", alloc::format!("Saved as {name} on the SD card."))),
            None => failed(cx, BUSY),
        }
    }

    fn levels<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let options = Level::PICKABLE.map(|level| (level.letter(), Msg::Level(level as u8)));
        let selected = Level::PICKABLE.iter().position(|&l| l == self.log.min_level()).unwrap_or(0);
        let body = ui.theme().fonts.body;
        segmented(ui, &options, selected).style(&TAB_TRACK).part(Part::Item, |item| item.style(&TAB).width(LEVEL_WIDTH)).part(Part::Label, |label| label.font(body)).shrink(0).id(id!("levels"))
    }

    fn header<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let (width, height) = ui.screen_size();
        let pause = if self.session.is_some() { icons::PAUSE } else { icons::PLAY };
        let actions = ui.row([
            icon_button(ui, icon(pause), Msg::Pause).id(id!("pause")),
            icon_button(ui, icon(icons::ERASER), Msg::Clear).id(id!("clear")),
            icon_button(ui, icon(icons::SAVE), Msg::Save).id(id!("save")),
        ]);
        let filter = text_field(ui, &self.filter, Msg::Input).placeholder("Filter").grow(1).id(id!("filter"));
        let header = if width > height {
            ui.row([top_bar(ui, Some(Msg::Back), "Logcat", ui.empty()), self.levels(ui), filter, actions]).gap(GAP).align(Align::Center).padding_each(0, 0, 0, 8)
        } else {
            ui.column([top_bar(ui, Some(Msg::Back), "Logcat", actions), ui.row([self.levels(ui), filter]).gap(GAP).align(Align::Center).padding_each(MARGIN, 0, MARGIN, GAP)])
        };
        header.padding_each(0, ui.insets().top, 0, 0).background(ui.colors().background).border(ui.colors().divider, 1).border_sides(Sides::BOTTOM)
    }

    fn row<'a>(&'a self, index: usize, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let Some(line) = self.log.shown_seq(index).and_then(|seq| self.log.get(seq)) else { return ui.empty() };
        let head = match line.header() {
            Some(h) => ui.format(format_args!("{} {:>5} {:>5} {}", h.time, h.pid, h.tid, h.tag)),
            None => "",
        };
        let top = ui.row([ui.text(&MONO, line.level.letter()).foreground(level_color(line.level)).shrink(0), ui.text(&MONO, head).foreground(colors::MUTED).ellipsis(Elide::End)]).gap(10);
        let message = ui.text(&MONO, line.message()).foreground(message_color(line.level)).ellipsis(Elide::End);
        let accent = if line.level >= Level::Warning { ui.column([]).width(4).background(level_color(line.level)).absolute(Some(0), Some(0), None, Some(0)) } else { ui.empty() };
        ui.column([top, message, accent])
            .justify(Justify::Center)
            .gap(2)
            .padding_x(ROW_PADDING)
            .border(ui.colors().divider, 1)
            .border_sides(Sides::BOTTOM)
            .when(State::PRESSED, &[Prop::Background(colors::PRESSED)])
            .on_tap(Msg::Open(index as u32))
    }

    fn lines<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let count = self.log.shown_len();
        let list = ui
            .collection(count, CollectionLayout::List { row_height: ROW_HEIGHT }, Self::row)
            .scroll_anchor(ScrollEdge::End)
            .on_scroll(Msg::Scrolled)
            .revision(self.revision)
            .id(id!("lines"))
            .grow(1);
        let mut children = vec![list];
        if count == 0 {
            let text = if self.log.len() == 0 { "No logs." } else { "No matching logs." };
            children.push(ui.column([ui.text(ui.theme().fonts.body, text).foreground(colors::MUTED)]).align(Align::Center).justify(Justify::Center).absolute(Some(0), Some(0), Some(0), Some(0)));
        } else if !self.at_end {
            let jump = ui
                .row([ui.icon(small_icon(icons::DOWN))])
                .size(JUMP_SIZE, JUMP_SIZE)
                .radius((JUMP_SIZE / 2) as u8)
                .align(Align::Center)
                .justify(Justify::Center)
                .background(ui.colors().surface)
                .foreground(ColorRole::Text)
                .shadow(Shadow { y: 2, blur: 8, color: colors::SHADOW })
                .when(State::PRESSED, &[Prop::Background(colors::PRESSED)])
                .on_tap(Msg::Latest)
                .absolute(None, None, Some(MARGIN), Some(MARGIN))
                .id(id!("latest"));
            children.push(jump);
        }
        ui.column(children).grow(1)
    }
}


/// Reads in one future while a timer hands what came in to the screen, so a
/// burst of lines costs one update per tick. A read is never cut short by a
/// timeout instead: dropped midway, it can lose the stream's acknowledgement
/// and stall it.
async fn session(this: This<LogcatScreen>, conn: Connection, command: String) {
    let mut stream = match adb::services::shell(&conn, &command).await {
        Ok(stream) => stream,
        Err(err) => {
            error!(TAG, "{command}: {err}");
            this.with(|s, cx| s.ended("[could not start logcat]", cx));
            return;
        }
    };
    let pending = RefCell::new((Vec::new(), false));
    let mut read = pin!(async {
        let mut buf = vec![0; READ_CHUNK];
        loop {
            match stream.read(&mut buf).await {
                Ok(0) => return Ok(()),
                Ok(n) => {
                    let mut p = pending.borrow_mut();
                    if p.0.len() + n > PENDING_MAX {
                        p.1 = true;
                    } else {
                        p.0.extend_from_slice(&buf[..n]);
                    }
                }
                Err(err) => return Err(err),
            }
        }
    });
    let mut tick = pin!(async {
        loop {
            executor::sleep(FLUSH).await;
            if !flush(&this, &pending) {
                return;
            }
        }
    });
    let end = poll_fn(|cx| {
        if let Poll::Ready(result) = read.as_mut().poll(cx) {
            return Poll::Ready(Some(result));
        }
        tick.as_mut().poll(cx).map(|()| None)
    })
    .await;
    let Some(result) = end else { return };
    if !flush(&this, &pending) {
        return;
    }
    if let Err(err) = &result {
        error!(TAG, "read: {err}");
    }
    this.with(|s, cx| s.ended("[logcat ended]", cx));
}

/// `false` once the screen has gone.
fn flush(this: &This<LogcatScreen>, pending: &RefCell<(Vec<u8>, bool)>) -> bool {
    let (data, dropped) = {
        let mut p = pending.borrow_mut();
        if p.0.is_empty() && !p.1 {
            return true;
        }
        (core::mem::take(&mut p.0), core::mem::take(&mut p.1))
    };
    this.with(|s, cx| s.receive(&data, dropped, cx)).is_some()
}

impl Component for LogcatScreen {
    type Msg = Msg;

    fn update(&mut self, msg: Msg, cx: &mut Cx<Self>) {
        match msg {
            Msg::Back => return cx.nav_of::<Root>().pop(),
            Msg::Level(i) => {
                self.log.set_min_level(Level::PICKABLE[usize::from(i)]);
                self.refiltered(cx);
            }
            Msg::Input(TextInput::Enter) => cx.focus(id!("logcat")),
            Msg::Input(input) => {
                if !self.filter.apply(input) {
                    return;
                }
                self.log.set_filter(Filter::parse(self.filter.as_str()));
                self.refiltered(cx);
            }
            Msg::Pause => match self.session.take() {
                Some(task) => task.abort(),
                None => self.start(cx),
            },
            Msg::Clear => {
                self.log.clear();
                self.revision += 1;
                self.at_end = true;
            }
            Msg::Save => self.save(cx),
            Msg::Open(index) => {
                if let Some(seq) = self.log.shown_seq(index as usize) {
                    self.open(seq, cx);
                }
            }
            Msg::Scrolled(position) => {
                self.offset = position.offset;
                let at_end = position.offset >= position.extent - i32::from(position.viewport) - 1;
                if at_end == self.at_end {
                    return;
                }
                self.at_end = at_end;
            }
            Msg::Latest => {
                self.at_end = true;
                cx.scroll_to(id!("lines"), ScrollTarget::End, false);
            }
            Msg::Step(seq, newer) => self.step(seq, newer, cx),
            Msg::FilterTag(seq) => {
                if let Some(text) = self.log.get(seq).and_then(|l| l.header()).map(|h| alloc::format!("tag:{}", h.tag)) {
                    self.set_filter_text(&text, cx);
                }
            }
            Msg::FilterPid(seq) => {
                if let Some(text) = self.log.get(seq).and_then(|l| l.header()).map(|h| alloc::format!("pid:{}", h.pid)) {
                    self.set_filter_text(&text, cx);
                }
            }
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        ui.column([self.header(ui), self.lines(ui), ui.column([]).height(ui.insets().bottom)]).background(ui.colors().surface).focusable().id(id!("logcat"))
    }
}

impl Screen for LogcatScreen {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        self.start(cx);
    }
}

#[derive(Clone, Copy)]
pub enum DetailMsg {
    Close,
    Older,
    Newer,
    Tag,
    Pid,
    /// Show `DetailLink::next`.
    Load,
}

struct DetailLine {
    seq: u32,
    level: Level,
    header: Option<(String, u32, u32, String)>,
    message: String,
    older: bool,
    newer: bool,
}

/// How the list reaches the sheet open over it, while it is open.
struct DetailLink {
    sheet: Option<Handle<DetailMsg>>,
    next: Option<DetailLine>,
}

/// One line's details, in a sheet over the list.
pub struct LogDetail {
    conn: Connection,
    line: DetailLine,
    process: Option<String>,
    process_task: Option<Task>,
    list: Handle<Msg>,
    link: Rc<RefCell<DetailLink>>,
}

impl LogDetail {
    fn new(conn: Connection, line: DetailLine, list: Handle<Msg>, link: Rc<RefCell<DetailLink>>) -> Self {
        Self { conn, line, process: None, process_task: None, list, link }
    }

    fn post(&self, msg: Msg) {
        if !self.list.post(msg) {
            error!(TAG, "detail: UI queue full");
        }
    }

    fn load_process(&mut self, cx: &mut Cx<Self>) {
        if let Some(task) = self.process_task.take() {
            task.abort();
        }
        self.process = None;
        if let Some((_, pid, _, _)) = &self.line.header {
            let (conn, pid) = (self.conn.clone(), *pid);
            self.process_task = Some(cx.spawn(move |this| load_process(this, conn, pid)));
        }
    }

    fn info<'a>(&'a self, ui: &'a Ui<DetailMsg>) -> Element<'a, DetailMsg> {
        let Some((time, pid, tid, _)) = &self.line.header else { return ui.empty() };
        let theme = ui.theme();
        let field = |label: &'a str, value: &'a str| ui.column([ui.text(theme.fonts.caption, label).foreground(colors::MUTED), ui.text(theme.fonts.body, value).ellipsis(Elide::End)]).gap(4);
        let process = field("Process", self.process.as_deref().unwrap_or(""));
        let (pid, tid) = (ui.format(format_args!("{pid}")), ui.format(format_args!("{tid}")));
        let (width, height) = ui.screen_size();
        if width > height {
            ui.column([ui.row([field("Time", time).width(TIME_WIDTH), field("PID", pid).width(ID_WIDTH), field("TID", tid).width(ID_WIDTH)]), process]).gap(GAP)
        } else {
            ui.column([field("Time", time), ui.row([field("PID", pid).width(0).grow(1), field("TID", tid).width(0).grow(1)]), process]).gap(GAP)
        }
    }

    fn actions<'a>(&'a self, ui: &'a Ui<DetailMsg>) -> Element<'a, DetailMsg> {
        let body = ui.theme().fonts.body;
        let step = |glyph: char, enabled: bool, msg: DetailMsg| ui.row([ui.icon(small_icon(glyph))]).style(&TONAL).size(ACTION_HEIGHT, ACTION_HEIGHT).align(Align::Center).justify(Justify::Center).disabled(!enabled).on_tap(msg);
        let filter = |label: &'a str, msg: DetailMsg| ui.row([ui.icon(small_icon(icons::FILTER)), ui.text(body, label)]).style(&TONAL).height(ACTION_HEIGHT).padding_x(20).gap(10).align(Align::Center).disabled(self.line.header.is_none()).on_tap(msg);
        let pid = match &self.line.header {
            Some((_, pid, _, _)) => ui.format(format_args!("PID {pid}")),
            None => "PID",
        };
        ui.row([
            step(icons::UP, self.line.older, DetailMsg::Older).id(id!("older")),
            step(icons::DOWN, self.line.newer, DetailMsg::Newer).id(id!("newer")),
            ui.column([]).grow(1),
            filter("This tag", DetailMsg::Tag).id(id!("this-tag")),
            filter(pid, DetailMsg::Pid).id(id!("this-pid")),
        ])
        .gap(12)
        .align(Align::Center)
    }
}

async fn load_process(this: This<LogDetail>, conn: Connection, pid: u32) {
    let name = match adb::services::exec(&conn, &alloc::format!("cat /proc/{pid}/cmdline 2>/dev/null")).await {
        Ok(out) => out.split(|&b| b == 0).next().map(|name| String::from_utf8_lossy(name).trim().to_string()).filter(|name| !name.is_empty()),
        Err(err) => {
            error!(TAG, "cmdline {pid}: {err}");
            None
        }
    };
    this.with(|s, _| s.process = Some(name.unwrap_or_else(|| String::from("-"))));
}

impl Component for LogDetail {
    type Msg = DetailMsg;

    fn update(&mut self, msg: DetailMsg, cx: &mut Cx<Self>) {
        match msg {
            DetailMsg::Close => cx.nav_of::<Overlay>().pop(),
            DetailMsg::Older => self.post(Msg::Step(self.line.seq, false)),
            DetailMsg::Newer => self.post(Msg::Step(self.line.seq, true)),
            DetailMsg::Tag => self.post(Msg::FilterTag(self.line.seq)),
            DetailMsg::Pid => self.post(Msg::FilterPid(self.line.seq)),
            DetailMsg::Load => {
                let Some(line) = self.link.borrow_mut().next.take() else { return };
                let pid = |line: &DetailLine| line.header.as_ref().map(|h| h.1);
                let same_process = pid(&line) == pid(&self.line);
                self.line = line;
                if !same_process {
                    self.load_process(cx);
                }
                cx.rebuild();
            }
        }
    }

    fn view<'a>(&'a self, ui: &'a Ui<DetailMsg>) -> Element<'a, DetailMsg> {
        let theme = ui.theme();
        let (width, height) = ui.screen_size();
        let insets = ui.insets();
        let badge = ui.row([ui.text(theme.fonts.body, self.line.level.name())]).height(44).padding_x(14).radius(10).align(Align::Center).background(tonal_color(self.line.level)).foreground(level_color(self.line.level)).shrink(0);
        let title = self.line.header.as_ref().map_or(self.line.level.name(), |(_, _, _, tag)| tag.as_str());
        let head = ui.row([badge, ui.text(theme.fonts.title, title).ellipsis(Elide::End).grow(1), icon_button(ui, icon(icons::CLOSE), DetailMsg::Close).id(id!("close"))]).gap(GAP).align(Align::Center);
        let message = ui.column([ui.scroll(ui.text(&MONO, &self.line.message).wrap().foreground(message_color(self.line.level))).max_height(MESSAGE_MAX_HEIGHT)]).padding(GAP).radius(GAP as u8).background(colors::LOG_MESSAGE);
        let sheet_width = if width > height { SHEET_WIDTH.min(width) } else { width };
        let sheet = ui
            .column([head, self.info(ui), message, self.actions(ui)])
            .gap(20)
            .width(sheet_width)
            .padding_each(MARGIN, MARGIN, MARGIN, MARGIN + insets.bottom)
            .background(ui.colors().surface)
            .radius(SHEET_RADIUS)
            .corners(Corners::TOP)
            .appear(Presence::from_edge(Edge::Bottom))
            .disappear(Presence::from_edge(Edge::Bottom))
            .id(id!("sheet"));
        ui.column([sheet]).align(Align::Center).justify(Justify::End).absolute(Some(0), Some(0), Some(0), Some(0))
    }
}

impl Screen for LogDetail {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        self.link.borrow_mut().sheet = Some(cx.handle());
        self.load_process(cx);
    }

    fn will_disappear(&mut self, _: &mut Cx<Self>) {
        let mut link = self.link.borrow_mut();
        link.sheet = None;
        link.next = None;
    }

    fn scrim_tap(&self) -> Option<DetailMsg> {
        Some(DetailMsg::Close)
    }
}
