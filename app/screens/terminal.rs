/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::String;
use alloc::vec::Vec;
use core::ffi::CStr;
use core::future::{Future, poll_fn};
use core::ops::Range;
use core::pin::pin;
use core::task::Poll;
use core::time::Duration;

use adb::link::Connection;
use adb::services::PtyOutput;
use devkit_gfx::{Color, TextOptions, TextRun};
use devkit_ui::widgets::{icon_button, top_bar};
use devkit_ui::{Canvas, Component, Cx, Element, Gesture, Gestures, Palette, Phase, Rect, Screen, TextInput, This, Ui, id};
use idf::executor::{self, Sender};
use idf::heap::Caps;
use idf::log::error;
use term_emu::{Cell, Key, Terminal, attr};

use crate::fonts::{MONO, icon};
use crate::resources::{TOUCH_TERMINAL_LANDSCAPE_KEYBOARD, TOUCH_TERMINAL_PORTRAIT_KEYBOARD, icons};

const TAG: &CStr = c"terminal";
const TERM: &str = "xterm-256color";
const HISTORY_LINES: usize = 1000;
const INPUT_QUEUE: usize = 64;
const PAD_X: i16 = 8;
const PAD_Y: i16 = 4;
const BLINK: Duration = Duration::from_millis(530);

/// The custom ids of keys on the terminal keyboards; F1 to F12 are 11 to 22.
const KEY_PAGE_UP: u16 = 1;
const KEY_PAGE_DOWN: u16 = 2;
const KEY_INSERT: u16 = 3;
const KEY_F1: u16 = 11;

/// xterm's first 16 colors, darkened where they would vanish on white.
const BASE16: [u32; 16] = [
    0x000000, 0xcc0000, 0x4e9a06, 0xc4a000, 0x3465a4, 0x75507b, 0x06989a, 0x999999, //
    0x555753, 0xef2929, 0x73d216, 0xedd400, 0x729fcf, 0xad7fa8, 0x2cc7c7, 0xb0b0b0,
];

#[derive(Clone, Copy)]
pub enum Msg {
    Back,
    ShowKeyboard,
    HideKeyboard,
    Resized(i16, i16),
    Input(TextInput),
    /// Distance dragged down since the finger landed; ended.
    Pan(i16, bool),
    None,
}

enum Input {
    Data(Vec<u8>),
    Resize(u16, u16),
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Ink {
    Foreground,
    Background,
    Index(u8),
}

#[derive(Clone, Copy, PartialEq, Eq)]
struct Look {
    fg: Ink,
    bg: Ink,
    dim: bool,
    underline: bool,
}

impl Look {
    fn of(cell: &Cell) -> Self {
        let mut fg = if cell.attr & attr::FG_DEFAULT != 0 {
            Ink::Foreground
        } else if cell.attr & attr::BOLD != 0 && cell.fg < 8 {
            Ink::Index(cell.fg + 8)
        } else {
            Ink::Index(cell.fg)
        };
        let mut bg = if cell.attr & attr::BG_DEFAULT != 0 { Ink::Background } else { Ink::Index(cell.bg) };
        if cell.attr & attr::REVERSE != 0 {
            core::mem::swap(&mut fg, &mut bg);
        }
        Self { fg, bg, dim: cell.attr & attr::DIM != 0, underline: cell.attr & attr::UNDERLINE != 0 }
    }
}

fn color(ink: Ink, dim: bool, palette: &Palette) -> Color {
    let base = match ink {
        Ink::Foreground => palette.text,
        Ink::Background => palette.surface,
        Ink::Index(i) => indexed(i),
    };
    if dim { palette.surface.mix(base, 128) } else { base }
}

fn indexed(i: u8) -> Color {
    match i {
        0..16 => Color::hex(BASE16[usize::from(i)]),
        16..232 => {
            let v = i - 16;
            let level = |n: u8| if n == 0 { 0 } else { 55 + n * 40 };
            Color::rgb(level(v / 36), level(v / 6 % 6), level(v % 6))
        }
        _ => {
            let g = 8 + (i - 232) * 10;
            Color::rgb(g, g, g)
        }
    }
}

/// One screen row, laid out: a run per foreground, with blanks where the
/// others are, so every run starts at the first column.
#[derive(Default)]
struct Line {
    fills: Vec<(Range<u16>, Ink)>,
    runs: Vec<(Ink, bool, TextRun<Caps>)>,
    underlines: Vec<(Range<u16>, Ink, bool)>,
}

impl Line {
    fn build(&mut self, cells: &[Cell], text: &mut String) {
        let looks: Vec<Look> = cells.iter().map(Look::of).collect();
        self.fills.clear();
        self.underlines.clear();
        spans(&looks, |l| (l.bg != Ink::Background).then_some(l.bg), |span, bg| self.fills.push((span, bg)));
        spans(&looks, |l| l.underline.then_some((l.fg, l.dim)), |span, (fg, dim)| self.underlines.push((span, fg, dim)));
        let mut inks: Vec<(Ink, bool)> = Vec::new();
        for (cell, look) in cells.iter().zip(&looks) {
            if cell.ch != b' ' && !inks.contains(&(look.fg, look.dim)) {
                inks.push((look.fg, look.dim));
            }
        }
        self.runs.truncate(inks.len());
        for (i, &(fg, dim)) in inks.iter().enumerate() {
            text.clear();
            text.extend(cells.iter().zip(&looks).map(|(cell, look)| if (look.fg, look.dim) == (fg, dim) { cell.ch as char } else { ' ' }));
            let trimmed = text.trim_end();
            match self.runs.get_mut(i) {
                Some(run) => {
                    (run.0, run.1) = (fg, dim);
                    run.2.layout(&MONO, trimmed, &TextOptions::default());
                }
                None => {
                    let mut run = TextRun::new_in(Caps::SPIRAM);
                    run.layout(&MONO, trimmed, &TextOptions::default());
                    self.runs.push((fg, dim, run));
                }
            }
        }
    }
}

/// Calls `found` for each stretch of cells `key` gives the same `Some` for.
fn spans<K: PartialEq + Copy>(looks: &[Look], key: impl Fn(&Look) -> Option<K>, mut found: impl FnMut(Range<u16>, K)) {
    let mut col = 0;
    while col < looks.len() {
        let Some(k) = key(&looks[col]) else {
            col += 1;
            continue;
        };
        let start = col;
        while col < looks.len() && key(&looks[col]) == Some(k) {
            col += 1;
        }
        found(start as u16..col as u16, k);
    }
}

/// The font's advance in 1/16 px, the unit text is laid out in.
fn measure_advance() -> i32 {
    let probe: String = core::iter::repeat_n('M', 257).collect();
    let mut run = TextRun::new();
    run.layout(&MONO, &probe, &TextOptions::default());
    let glyphs = run.glyphs();
    i32::from(glyphs[256].x - glyphs[0].x) / 16
}

pub struct TerminalScreen {
    conn: Connection,
    term: Terminal<Caps>,
    advance: i32,
    lines: Vec<Line>,
    cursor_run: TextRun<Caps>,
    text: String,
    size: (i16, i16),
    sized: bool,
    input: Option<Sender<Input>>,
    offset: usize,
    history_len: usize,
    drawn_cursor: Option<(usize, usize)>,
    blink_on: bool,
    pan_from: i16,
}

impl TerminalScreen {
    pub fn new(conn: Connection) -> Self {
        Self {
            conn,
            term: Terminal::new_in(2, 1, HISTORY_LINES, Caps::SPIRAM),
            advance: measure_advance(),
            lines: Vec::new(),
            cursor_run: TextRun::new_in(Caps::SPIRAM),
            text: String::new(),
            size: (0, 0),
            sized: false,
            input: None,
            offset: 0,
            history_len: 0,
            drawn_cursor: None,
            blink_on: true,
            pan_from: 0,
        }
    }

    fn x(&self, col: usize) -> i16 {
        PAD_X + ((col as i32 * self.advance + 8) >> 4) as i16
    }

    fn y(&self, row: usize) -> i16 {
        PAD_Y + row as i16 * MONO.line_height()
    }

    fn rows_rect(&self, rows: Range<usize>) -> Rect {
        Rect::new(0, self.y(rows.start), self.size.0, (rows.end - rows.start) as i16 * MONO.line_height())
    }

    fn cell_rect(&self, (row, col): (usize, usize)) -> Rect {
        Rect::new(self.x(col), self.y(row), self.x(col + 1) - self.x(col), MONO.line_height())
    }

    fn cursor(&self) -> Option<(usize, usize)> {
        (self.offset == 0 && self.term.cursor_visible()).then(|| self.term.cursor())
    }

    fn send(&self, input: Input) {
        let Some(tx) = &self.input else { return };
        if tx.try_send(input).is_err() {
            error!(TAG, "input dropped");
        }
    }

    fn resize(&mut self, width: i16, height: i16, cx: &mut Cx<Self>) {
        self.size = (width, height);
        let cols = (i32::from(width - 2 * PAD_X) * 16 / self.advance).max(2) as usize;
        let rows = ((height - 2 * PAD_Y) / MONO.line_height()).max(1) as usize;
        self.term.resize(cols, rows);
        self.lines.resize_with(self.term.rows(), Line::default);
        self.sized = true;
        self.send(Input::Resize(self.term.rows() as u16, self.term.cols() as u16));
        self.refresh(true, cx);
    }

    fn output(&mut self, data: &[u8], cx: &mut Cx<Self>) {
        self.term.feed(data);
        let response = self.term.take_response();
        if !response.is_empty() {
            self.send(Input::Data(response));
        }
        self.refresh(false, cx);
    }

    fn closed(&mut self, cx: &mut Cx<Self>) {
        self.input = None;
        self.output(b"\x18\r\n[shell closed]\r\n", cx);
    }

    fn set_offset(&mut self, offset: usize, cx: &mut Cx<Self>) {
        let offset = offset.min(self.term.history_len());
        if offset != self.offset {
            self.offset = offset;
            self.refresh(true, cx);
        }
    }

    /// Lays out what changed and repaints it. A view scrolled back stays on
    /// the same lines while output arrives.
    fn refresh(&mut self, mut all: bool, cx: &mut Cx<Self>) {
        let history_len = self.term.history_len();
        if self.offset > 0 && history_len > self.history_len {
            self.offset = (self.offset + history_len - self.history_len).min(history_len);
            all = true;
        }
        self.offset = self.offset.min(history_len);
        self.history_len = history_len;
        let dirty = self.term.take_dirty();
        let rows = match dirty {
            _ if all => 0..self.lines.len(),
            Some(_) if self.offset > 0 => 0..self.lines.len(),
            Some(rows) => rows.start..rows.end.min(self.lines.len()),
            None => 0..0,
        };
        for row in rows.clone() {
            self.lines[row].build(self.term.row(row, self.offset), &mut self.text);
        }
        if !rows.is_empty() {
            cx.redraw_rect(id!("term"), self.rows_rect(rows));
        }
        let cursor = self.cursor();
        if cursor != self.drawn_cursor {
            if let Some(old) = self.drawn_cursor {
                cx.redraw_rect(id!("term"), self.cell_rect(old));
            }
            self.drawn_cursor = cursor;
        }
        if let Some(at) = cursor {
            let ch = self.term.row(at.0, 0)[at.1].ch;
            self.text.clear();
            self.text.push(ch as char);
            self.cursor_run.layout(&MONO, &self.text, &TextOptions::default());
            self.blink_on = true;
            cx.redraw_rect(id!("term"), self.cell_rect(at));
        }
    }

    fn blink(&mut self, cx: &mut Cx<Self>) {
        self.blink_on = !self.blink_on;
        if let Some(at) = self.drawn_cursor {
            cx.redraw_rect(id!("term"), self.cell_rect(at));
        }
    }

    fn key(&mut self, input: TextInput, cx: &mut Cx<Self>) {
        let key = match input {
            TextInput::Char(c) => {
                let mut buf = [0; 4];
                self.typed(c.encode_utf8(&mut buf).as_bytes(), cx);
                return;
            }
            TextInput::Backspace => Key::Backspace,
            TextInput::Delete => Key::Delete,
            TextInput::Enter => Key::Enter,
            TextInput::Left => Key::Left,
            TextInput::Right => Key::Right,
            TextInput::Up => Key::Up,
            TextInput::Down => Key::Down,
            TextInput::Home => Key::Home,
            TextInput::End => Key::End,
            TextInput::Custom(KEY_PAGE_UP) => Key::PageUp,
            TextInput::Custom(KEY_PAGE_DOWN) => Key::PageDown,
            TextInput::Custom(KEY_INSERT) => Key::Insert,
            TextInput::Custom(id @ KEY_F1..=22) => Key::F((id - KEY_F1 + 1) as u8),
            TextInput::Custom(_) | TextInput::Place(_) => return,
        };
        let bytes = self.term.key(key);
        self.typed(bytes, cx);
    }

    fn typed(&mut self, bytes: &[u8], cx: &mut Cx<Self>) {
        self.send(Input::Data(bytes.to_vec()));
        self.set_offset(0, cx);
    }

    fn pan(&mut self, dy: i16, ended: bool, cx: &mut Cx<Self>) {
        let line = MONO.line_height();
        let lines = (dy - self.pan_from) / line;
        if lines != 0 {
            self.pan_from += lines * line;
            let offset = (self.offset as isize + lines as isize).max(0) as usize;
            self.set_offset(offset, cx);
        }
        if ended {
            self.pan_from = 0;
        }
    }

    fn draw<'a>(&'a self, canvas: &mut Canvas<'a, '_>) {
        let colors = canvas.colors();
        let (width, height) = canvas.size();
        canvas.fill(Rect::new(0, 0, width, height), colors.surface);
        let line = MONO.line_height();
        for (row, l) in self.lines.iter().enumerate() {
            let y = self.y(row);
            if !canvas.needs(Rect::new(0, y, width, line)) {
                continue;
            }
            for (cols, ink) in &l.fills {
                let x = self.x(usize::from(cols.start));
                canvas.fill(Rect::new(x, y, self.x(usize::from(cols.end)) - x, line), color(*ink, false, &colors));
            }
            for (fg, dim, run) in &l.runs {
                canvas.text(PAD_X, y, run, color(*fg, *dim, &colors));
            }
            for (cols, fg, dim) in &l.underlines {
                let x = self.x(usize::from(cols.start));
                canvas.fill(Rect::new(x, y + line - 2, self.x(usize::from(cols.end)) - x, 1), color(*fg, *dim, &colors));
            }
        }
        if let Some(at) = self.drawn_cursor
            && self.blink_on
        {
            let look = Look::of(&self.term.row(at.0, 0)[at.1]);
            let rect = self.cell_rect(at);
            canvas.fill(rect, color(look.fg, false, &colors));
            canvas.text(rect.x, rect.y, &self.cursor_run, color(look.bg, false, &colors));
        }
    }
}

fn pan(gesture: Gesture) -> Msg {
    match gesture {
        Gesture::Pan { phase, dy, .. } => Msg::Pan(dy, phase == Phase::Ended),
        _ => Msg::None,
    }
}

async fn session(this: This<TerminalScreen>, conn: Connection) {
    let (mut reader, mut writer) = match adb::services::pty(&conn, TERM, "").await {
        Ok(pty) => pty,
        Err(err) => {
            error!(TAG, "pty: {err}");
            this.with(|s, cx| s.output(b"[could not start a shell]\r\n", cx));
            return;
        }
    };
    let (tx, mut rx) = executor::channel(INPUT_QUEUE);
    let started = this.with(|s, _| {
        s.input = Some(tx);
        if s.sized {
            s.send(Input::Resize(s.term.rows() as u16, s.term.cols() as u16));
        }
    });
    if started.is_none() {
        return;
    }
    let mut write = pin!(async move {
        loop {
            let result = match rx.recv().await {
                Input::Data(data) => writer.write(&data).await,
                Input::Resize(rows, cols) => writer.resize(rows, cols).await,
            };
            if let Err(err) = result {
                return err;
            }
        }
    });
    let mut read = pin!(async {
        loop {
            match reader.read().await {
                Ok(PtyOutput::Data(data)) => {
                    if this.with(|s, cx| s.output(&data, cx)).is_none() {
                        return Ok(());
                    }
                }
                Ok(PtyOutput::Exit(_)) => return Ok(()),
                Err(err) => return Err(err),
            }
        }
    });
    let end = poll_fn(|cx| {
        if let Poll::Ready(result) = read.as_mut().poll(cx) {
            return Poll::Ready(result);
        }
        write.as_mut().poll(cx).map(Err)
    })
    .await;
    if let Err(err) = end
        && !matches!(err, adb::Error::Closed)
    {
        error!(TAG, "session: {err}");
    }
    this.with(|s, cx| s.closed(cx));
}

async fn blink(this: This<TerminalScreen>) {
    loop {
        executor::sleep(BLINK).await;
        if this.with(|s, cx| s.blink(cx)).is_none() {
            return;
        }
    }
}

impl Component for TerminalScreen {
    type Msg = Msg;

    fn update(&mut self, msg: Msg, cx: &mut Cx<Self>) {
        match msg {
            Msg::Back => cx.nav().pop(),
            Msg::ShowKeyboard => cx.focus(id!("term")),
            Msg::HideKeyboard => cx.focus(id!("terminal")),
            Msg::Resized(width, height) => self.resize(width, height, cx),
            Msg::Input(input) => self.key(input, cx),
            Msg::Pan(dy, ended) => self.pan(dy, ended, cx),
            Msg::None => {}
        }
    }

    fn view<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let (width, height) = ui.screen_size();
        let keyboard = if width > height { &TOUCH_TERMINAL_LANDSCAPE_KEYBOARD } else { &TOUCH_TERMINAL_PORTRAIT_KEYBOARD };
        let insets = ui.insets();
        let toggle = if insets.bottom > 0 { icon_button(ui, icon(icons::KEYBOARD_HIDE), Msg::HideKeyboard) } else { icon_button(ui, icon(icons::KEYBOARD), Msg::ShowKeyboard) };
        let term = ui
            .canvas(Self::draw)
            .grow(1)
            .on_size(Msg::Resized)
            .on_input(Msg::Input)
            .keyboard(keyboard)
            .gestures(Gestures::PAN, pan)
            .task(|this: This<Self>| blink(this))
            .id(id!("term"));
        ui.column([top_bar(ui, Some(Msg::Back), "Terminal", toggle), term])
            .padding_each(0, insets.top, 0, insets.bottom)
            .background(ui.colors().background)
            .focusable()
            .id(id!("terminal"))
    }
}

impl Screen for TerminalScreen {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        let conn = self.conn.clone();
        cx.spawn(|this| session(this, conn));
    }

    fn will_appear(&mut self, cx: &mut Cx<Self>) {
        cx.focus(id!("term"));
    }
}
