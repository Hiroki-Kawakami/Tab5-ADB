/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * A VT100/xterm-subset terminal emulator: output bytes in, a cell grid out.
 */

#![cfg_attr(not(test), no_std)]

extern crate alloc;

mod parser;
#[cfg(test)]
mod tests;

use alloc::alloc::{Allocator, Global};
use alloc::vec::Vec;
use core::ops::Range;

use parser::Parser;

pub mod attr {
    pub const BOLD: u8 = 1 << 0;
    pub const DIM: u8 = 1 << 1;
    pub const REVERSE: u8 = 1 << 2;
    pub const UNDERLINE: u8 = 1 << 3;
    /// `fg` means nothing: the theme's foreground.
    pub const FG_DEFAULT: u8 = 1 << 4;
    /// `bg` means nothing: the theme's background.
    pub const BG_DEFAULT: u8 = 1 << 5;
}

/// One character cell. Colors are xterm 256-color indices.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Cell {
    /// Printable ASCII; `?` stands for any other character, followed by a
    /// blank cell when it is a wide one.
    pub ch: u8,
    pub fg: u8,
    pub bg: u8,
    pub attr: u8,
}

impl Cell {
    pub const BLANK: Cell = Cell { ch: b' ', fg: 0, bg: 0, attr: attr::FG_DEFAULT | attr::BG_DEFAULT };
}

/// Keys that send a sequence rather than a character.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Key {
    Escape,
    Tab,
    Backspace,
    Enter,
    Up,
    Down,
    Right,
    Left,
    Home,
    End,
    PageUp,
    PageDown,
    Insert,
    Delete,
    /// F1 to F12.
    F(u8),
}

const MAX_COLS: usize = 512;
const MAX_ROWS: usize = 256;

/// DEC special graphics 0x60..=0x7e in ASCII, as the font has no box drawing.
const DEC_GRAPHICS: &[u8; 31] = b"+#    '#  +++++----_++++|<>*!#.";

#[derive(Clone, Copy, PartialEq, Eq)]
struct Pen {
    fg: u8,
    bg: u8,
    attr: u8,
}

impl Pen {
    const DEFAULT: Pen = Pen { fg: 0, bg: 0, attr: attr::FG_DEFAULT | attr::BG_DEFAULT };
}

#[derive(Clone, Copy)]
struct Saved {
    row: usize,
    col: usize,
    pen: Pen,
    dec_graphics: bool,
}

impl Saved {
    const HOME: Saved = Saved { row: 0, col: 0, pen: Pen::DEFAULT, dec_graphics: false };
}

struct Grid<A: Allocator> {
    cols: usize,
    cells: Vec<Cell, A>,
    order: Vec<u16>,
}

impl<A: Allocator> Grid<A> {
    fn new_in(cols: usize, rows: usize, alloc: A) -> Self {
        let mut cells = Vec::with_capacity_in(cols * rows, alloc);
        cells.resize(cols * rows, Cell::BLANK);
        Self { cols, cells, order: (0..rows as u16).collect() }
    }

    fn row(&self, row: usize) -> &[Cell] {
        let start = usize::from(self.order[row]) * self.cols;
        &self.cells[start..start + self.cols]
    }

    fn row_mut(&mut self, row: usize) -> &mut [Cell] {
        let start = usize::from(self.order[row]) * self.cols;
        &mut self.cells[start..start + self.cols]
    }
}

/// Lines scrolled off the top of the main screen, oldest dropped first.
struct History<A: Allocator> {
    cols: usize,
    capacity: usize,
    cells: Vec<Cell, A>,
    head: usize,
    len: usize,
}

impl<A: Allocator> History<A> {
    fn new_in(cols: usize, capacity: usize, alloc: A) -> Self {
        let mut cells = Vec::with_capacity_in(cols * capacity, alloc);
        cells.resize(cols * capacity, Cell::BLANK);
        Self { cols, capacity, cells, head: 0, len: 0 }
    }

    fn push(&mut self, row: &[Cell]) {
        if self.capacity == 0 {
            return;
        }
        let start = self.head * self.cols;
        copy_row(&mut self.cells[start..start + self.cols], row);
        self.head = (self.head + 1) % self.capacity;
        self.len = (self.len + 1).min(self.capacity);
    }

    /// `back` 1 is the newest line.
    fn line(&self, back: usize) -> &[Cell] {
        let slot = (self.head + self.capacity - back) % self.capacity;
        &self.cells[slot * self.cols..(slot + 1) * self.cols]
    }

    fn drop_newest(&mut self, n: usize) {
        let n = n.min(self.len);
        self.head = (self.head + self.capacity - n) % self.capacity;
        self.len -= n;
    }

    fn clear(&mut self) {
        self.head = 0;
        self.len = 0;
    }
}

fn copy_row(to: &mut [Cell], from: &[Cell]) {
    let n = to.len().min(from.len());
    to[..n].copy_from_slice(&from[..n]);
    to[n..].fill(Cell::BLANK);
}

/// Coarse wcwidth == 2 ranges, to keep the cursor column in step with the
/// device's.
fn is_wide(cp: u32) -> bool {
    matches!(cp, 0x1100..=0x115f | 0x2e80..=0xa4cf | 0xac00..=0xd7a3 | 0xf900..=0xfaff | 0xfe30..=0xfe4f | 0xff00..=0xff60 | 0xffe0..=0xffe6 | 0x1f300..=0x1f64f | 0x20000..=0x3fffd)
}

pub struct Terminal<A: Allocator + Clone = Global> {
    alloc: A,
    cols: usize,
    rows: usize,
    main: Grid<A>,
    alt: Grid<A>,
    alt_active: bool,
    history: History<A>,
    row: usize,
    col: usize,
    wrap_pending: bool,
    pen: Pen,
    scroll_top: usize,
    scroll_bottom: usize,
    cursor_visible: bool,
    app_cursor: bool,
    autowrap: bool,
    insert: bool,
    dec_graphics: bool,
    last_printed: u8,
    saved: Saved,
    alt_saved: Saved,
    dirty: Option<Range<usize>>,
    response: Vec<u8>,
    parser: Parser,
}

impl Terminal<Global> {
    pub fn new(cols: usize, rows: usize, scrollback: usize) -> Self {
        Self::new_in(cols, rows, scrollback, Global)
    }
}

impl<A: Allocator + Clone> Terminal<A> {
    /// The grids and `scrollback` lines of history come from `alloc`.
    pub fn new_in(cols: usize, rows: usize, scrollback: usize, alloc: A) -> Self {
        let cols = cols.clamp(2, MAX_COLS);
        let rows = rows.clamp(1, MAX_ROWS);
        Self {
            main: Grid::new_in(cols, rows, alloc.clone()),
            alt: Grid::new_in(cols, rows, alloc.clone()),
            history: History::new_in(cols, scrollback, alloc.clone()),
            alloc,
            cols,
            rows,
            alt_active: false,
            row: 0,
            col: 0,
            wrap_pending: false,
            pen: Pen::DEFAULT,
            scroll_top: 0,
            scroll_bottom: rows - 1,
            cursor_visible: true,
            app_cursor: false,
            autowrap: true,
            insert: false,
            dec_graphics: false,
            last_printed: b' ',
            saved: Saved::HOME,
            alt_saved: Saved::HOME,
            dirty: Some(0..rows),
            response: Vec::new(),
            parser: Parser::new(),
        }
    }

    pub fn cols(&self) -> usize {
        self.cols
    }

    pub fn rows(&self) -> usize {
        self.rows
    }

    /// Row `row` of the view `offset` lines back into the history, which the
    /// alternate screen has none of.
    pub fn row(&self, row: usize, offset: usize) -> &[Cell] {
        let row = row.min(self.rows - 1);
        if self.alt_active {
            return self.alt.row(row);
        }
        let offset = offset.min(self.history.len);
        if row >= offset { self.main.row(row - offset) } else { self.history.line(offset - row) }
    }

    pub fn history_len(&self) -> usize {
        if self.alt_active { 0 } else { self.history.len }
    }

    pub fn alt_screen(&self) -> bool {
        self.alt_active
    }

    /// Screen rows changed since the last call.
    pub fn take_dirty(&mut self) -> Option<Range<usize>> {
        self.dirty.take()
    }

    /// Row and column, on the screen.
    pub fn cursor(&self) -> (usize, usize) {
        (self.row, self.col.min(self.cols - 1))
    }

    pub fn cursor_visible(&self) -> bool {
        self.cursor_visible
    }

    /// What the terminal answers the host, such as a cursor position report.
    pub fn take_response(&mut self) -> Vec<u8> {
        core::mem::take(&mut self.response)
    }

    pub fn key(&self, key: Key) -> &'static [u8] {
        let app = self.app_cursor;
        match key {
            Key::Escape => b"\x1b",
            Key::Tab => b"\t",
            Key::Backspace => b"\x7f",
            Key::Enter => b"\r",
            Key::Up if app => b"\x1bOA",
            Key::Up => b"\x1b[A",
            Key::Down if app => b"\x1bOB",
            Key::Down => b"\x1b[B",
            Key::Right if app => b"\x1bOC",
            Key::Right => b"\x1b[C",
            Key::Left if app => b"\x1bOD",
            Key::Left => b"\x1b[D",
            Key::Home if app => b"\x1bOH",
            Key::Home => b"\x1b[H",
            Key::End if app => b"\x1bOF",
            Key::End => b"\x1b[F",
            Key::PageUp => b"\x1b[5~",
            Key::PageDown => b"\x1b[6~",
            Key::Insert => b"\x1b[2~",
            Key::Delete => b"\x1b[3~",
            Key::F(n) => match n {
                1 => b"\x1bOP",
                2 => b"\x1bOQ",
                3 => b"\x1bOR",
                4 => b"\x1bOS",
                5 => b"\x1b[15~",
                6 => b"\x1b[17~",
                7 => b"\x1b[18~",
                8 => b"\x1b[19~",
                9 => b"\x1b[20~",
                10 => b"\x1b[21~",
                11 => b"\x1b[23~",
                12 => b"\x1b[24~",
                _ => b"",
            },
        }
    }

    /// Clears both screens, the history, modes and attributes.
    pub fn reset(&mut self) {
        for row in 0..self.rows {
            self.main.row_mut(row).fill(Cell::BLANK);
            self.alt.row_mut(row).fill(Cell::BLANK);
        }
        self.alt_active = false;
        self.history.clear();
        self.row = 0;
        self.col = 0;
        self.wrap_pending = false;
        self.pen = Pen::DEFAULT;
        self.scroll_top = 0;
        self.scroll_bottom = self.rows - 1;
        self.cursor_visible = true;
        self.app_cursor = false;
        self.autowrap = true;
        self.insert = false;
        self.dec_graphics = false;
        self.last_printed = b' ';
        self.saved = Saved::HOME;
        self.alt_saved = Saved::HOME;
        self.parser = Parser::new();
        self.mark_all_dirty();
    }

    /// Keeps the cursor's line on the main screen: shrinking pushes lines
    /// above it into the history, growing brings history lines back.
    pub fn resize(&mut self, cols: usize, rows: usize) {
        let cols = cols.clamp(2, MAX_COLS);
        let rows = rows.clamp(1, MAX_ROWS);
        if cols == self.cols && rows == self.rows {
            return;
        }
        if cols != self.history.cols {
            let mut history = History::new_in(cols, self.history.capacity, self.alloc.clone());
            for back in (1..=self.history.len).rev() {
                history.push(self.history.line(back));
            }
            self.history = history;
        }
        let main_row = if self.alt_active { self.alt_saved.row } else { self.row };
        let drop = (main_row + 1).saturating_sub(rows);
        let pull = rows.saturating_sub(self.rows).min(self.history.len);
        for row in 0..drop {
            self.history.push(self.main.row(row));
        }
        let mut main = Grid::new_in(cols, rows, self.alloc.clone());
        for row in 0..rows {
            if row < pull {
                copy_row(main.row_mut(row), self.history.line(pull - row));
            } else if row - pull + drop < self.rows {
                copy_row(main.row_mut(row), self.main.row(row - pull + drop));
            }
        }
        self.history.drop_newest(pull);
        let mut alt = Grid::new_in(cols, rows, self.alloc.clone());
        for row in 0..rows.min(self.rows) {
            copy_row(alt.row_mut(row), self.alt.row(row));
        }
        self.main = main;
        self.alt = alt;
        let shift = |row: usize| (row + pull).saturating_sub(drop).min(rows - 1);
        if self.alt_active {
            self.alt_saved.row = shift(self.alt_saved.row);
            self.row = self.row.min(rows - 1);
        } else {
            self.row = shift(self.row);
        }
        self.col = self.col.min(cols - 1);
        for saved in [&mut self.saved, &mut self.alt_saved] {
            saved.row = saved.row.min(rows - 1);
            saved.col = saved.col.min(cols - 1);
        }
        self.cols = cols;
        self.rows = rows;
        self.scroll_top = 0;
        self.scroll_bottom = rows - 1;
        self.wrap_pending = false;
        self.mark_all_dirty();
    }

    fn screen(&mut self) -> &mut Grid<A> {
        if self.alt_active { &mut self.alt } else { &mut self.main }
    }

    fn mark_dirty(&mut self, rows: Range<usize>) {
        self.dirty = Some(match self.dirty.take() {
            Some(d) => d.start.min(rows.start)..d.end.max(rows.end),
            None => rows,
        });
    }

    fn mark_all_dirty(&mut self) {
        self.dirty = Some(0..self.rows);
    }

    /// Erased cells keep the background (BCE) but no other attribute.
    fn blank(&self) -> Cell {
        Cell { ch: b' ', fg: 0, bg: self.pen.bg, attr: (self.pen.attr & attr::BG_DEFAULT) | attr::FG_DEFAULT }
    }

    fn erase(&mut self, row: usize, cols: Range<usize>) {
        let blank = self.blank();
        self.screen().row_mut(row)[cols].fill(blank);
        self.mark_dirty(row..row + 1);
    }

    fn erase_rows(&mut self, rows: Range<usize>) {
        let cols = self.cols;
        for row in rows {
            self.erase(row, 0..cols);
        }
    }

    fn scroll_up(&mut self, n: usize) {
        let (top, bottom) = (self.scroll_top, self.scroll_bottom);
        let n = n.clamp(1, bottom - top + 1);
        if !self.alt_active && top == 0 {
            for row in 0..n {
                self.history.push(self.main.row(row));
            }
        }
        self.screen().order[top..=bottom].rotate_left(n);
        self.erase_rows(bottom + 1 - n..bottom + 1);
        self.mark_dirty(top..bottom + 1);
    }

    fn scroll_down(&mut self, n: usize) {
        let (top, bottom) = (self.scroll_top, self.scroll_bottom);
        let n = n.clamp(1, bottom - top + 1);
        self.screen().order[top..=bottom].rotate_right(n);
        self.erase_rows(top..top + n);
        self.mark_dirty(top..bottom + 1);
    }

    fn move_to(&mut self, row: isize, col: isize) {
        self.row = row.clamp(0, self.rows as isize - 1) as usize;
        self.col = col.clamp(0, self.cols as isize - 1) as usize;
        self.wrap_pending = false;
    }

    fn line_feed(&mut self) {
        self.wrap_pending = false;
        if self.row == self.scroll_bottom {
            self.scroll_up(1);
        } else if self.row < self.rows - 1 {
            self.row += 1;
        }
    }

    fn reverse_line_feed(&mut self) {
        self.wrap_pending = false;
        if self.row == self.scroll_top {
            self.scroll_down(1);
        } else if self.row > 0 {
            self.row -= 1;
        }
    }

    fn carriage_return(&mut self) {
        self.col = 0;
        self.wrap_pending = false;
    }

    fn put(&mut self, ch: u8) {
        if self.wrap_pending {
            self.carriage_return();
            self.line_feed();
        }
        let (row, col, cols, insert) = (self.row, self.col, self.cols, self.insert);
        let cell = Cell { ch, fg: self.pen.fg, bg: self.pen.bg, attr: self.pen.attr };
        let line = self.screen().row_mut(row);
        if insert {
            line.copy_within(col..cols - 1, col + 1);
        }
        line[col] = cell;
        self.mark_dirty(row..row + 1);
        if col == cols - 1 {
            self.wrap_pending = self.autowrap;
        } else {
            self.col += 1;
        }
        self.last_printed = ch;
    }

    fn print(&mut self, cp: u32) {
        match cp {
            0..=0x1f | 0x7f => {}
            0x60..=0x7e if self.dec_graphics => self.put(DEC_GRAPHICS[(cp - 0x60) as usize]),
            0x20..=0x7e => self.put(cp as u8),
            _ => {
                self.put(b'?');
                if is_wide(cp) {
                    self.put(b' ');
                }
            }
        }
    }

    fn execute(&mut self, c: u8) {
        match c {
            0x08 => {
                self.col = self.col.saturating_sub(1);
                self.wrap_pending = false;
            }
            0x09 => self.col = ((self.col / 8 + 1) * 8).min(self.cols - 1),
            0x0a..=0x0c => self.line_feed(),
            0x0d => self.carriage_return(),
            _ => {}
        }
    }

    fn save_cursor(&self) -> Saved {
        Saved { row: self.row, col: self.col, pen: self.pen, dec_graphics: self.dec_graphics }
    }

    fn restore_cursor(&mut self, saved: Saved) {
        self.move_to(saved.row as isize, saved.col as isize);
        self.pen = saved.pen;
        self.dec_graphics = saved.dec_graphics;
    }

    fn esc_dispatch(&mut self, intermediate: u8, last: u8) {
        if intermediate == b'(' {
            match last {
                b'0' => self.dec_graphics = true,
                b'B' => self.dec_graphics = false,
                _ => {}
            }
            return;
        }
        if intermediate != 0 {
            return;
        }
        match last {
            b'7' => self.saved = self.save_cursor(),
            b'8' => self.restore_cursor(self.saved),
            b'D' => self.line_feed(),
            b'E' => {
                self.carriage_return();
                self.line_feed();
            }
            b'M' => self.reverse_line_feed(),
            b'c' => self.reset(),
            _ => {}
        }
    }

    fn csi_dispatch(&mut self, private: u8, params: &[u32], intermediate: u8, last: u8) {
        if intermediate != 0 {
            return;
        }
        let p = |i: usize, default: u32| params.get(i).copied().filter(|&v| v != 0).unwrap_or(default) as isize;
        let (row, col) = (self.row as isize, self.col as isize);
        match private {
            b'?' => {
                match last {
                    b'h' | b'l' => {
                        for i in 0..params.len().max(1) {
                            self.set_mode(params.get(i).copied().unwrap_or(0), true, last == b'h');
                        }
                    }
                    b'c' => self.respond(b"\x1b[?6c"),
                    _ => {}
                }
                return;
            }
            b'>' => {
                if last == b'c' {
                    self.respond(b"\x1b[>0;0;0c");
                }
                return;
            }
            0 => {}
            _ => return,
        }
        match last {
            b'A' => {
                let top = if self.row >= self.scroll_top { self.scroll_top } else { 0 };
                self.move_to((row - p(0, 1)).max(top as isize), col);
            }
            b'B' | b'e' => {
                let bottom = if self.row <= self.scroll_bottom { self.scroll_bottom } else { self.rows - 1 };
                self.move_to((row + p(0, 1)).min(bottom as isize), col);
            }
            b'C' | b'a' => self.move_to(row, col + p(0, 1)),
            b'D' => self.move_to(row, col - p(0, 1)),
            b'E' => self.move_to(row + p(0, 1), 0),
            b'F' => self.move_to(row - p(0, 1), 0),
            b'G' | b'`' => self.move_to(row, p(0, 1) - 1),
            b'H' | b'f' => self.move_to(p(0, 1) - 1, p(1, 1) - 1),
            b'd' => self.move_to(p(0, 1) - 1, col),
            b'J' => {
                self.wrap_pending = false;
                let (row, col, rows, cols) = (self.row, self.col, self.rows, self.cols);
                match params.first().copied().unwrap_or(0) {
                    0 => {
                        self.erase(row, col..cols);
                        self.erase_rows(row + 1..rows);
                    }
                    1 => {
                        self.erase_rows(0..row);
                        self.erase(row, 0..col + 1);
                    }
                    2 => self.erase_rows(0..rows),
                    3 => self.history.clear(),
                    _ => {}
                }
            }
            b'K' => {
                self.wrap_pending = false;
                let (row, col, cols) = (self.row, self.col, self.cols);
                match params.first().copied().unwrap_or(0) {
                    0 => self.erase(row, col..cols),
                    1 => self.erase(row, 0..col + 1),
                    2 => self.erase(row, 0..cols),
                    _ => {}
                }
            }
            b'@' | b'P' | b'X' => {
                let (row, col, cols) = (self.row, self.col, self.cols);
                let n = (p(0, 1) as usize).clamp(1, cols - col);
                let line = self.screen().row_mut(row);
                match last {
                    b'@' => {
                        line.copy_within(col..cols - n, col + n);
                        self.erase(row, col..col + n);
                    }
                    b'P' => {
                        line.copy_within(col + n..cols, col);
                        self.erase(row, cols - n..cols);
                    }
                    _ => self.erase(row, col..col + n),
                }
            }
            b'L' | b'M' => {
                if (self.scroll_top..=self.scroll_bottom).contains(&self.row) {
                    let top = self.scroll_top;
                    self.scroll_top = self.row;
                    if last == b'L' {
                        self.scroll_down(p(0, 1) as usize);
                    } else {
                        self.scroll_up(p(0, 1) as usize);
                    }
                    self.scroll_top = top;
                    self.col = 0;
                    self.wrap_pending = false;
                }
            }
            b'S' => self.scroll_up(p(0, 1) as usize),
            b'T' => self.scroll_down(p(0, 1) as usize),
            b'b' => {
                let n = (p(0, 1) as usize).min(self.cols * self.rows);
                for _ in 0..n {
                    self.put(self.last_printed);
                }
            }
            b'm' => self.sgr(params),
            b'r' => {
                let last_row = self.rows as isize - 1;
                let top = (p(0, 1) - 1).clamp(0, last_row) as usize;
                let bottom = (p(1, self.rows as u32) - 1).clamp(0, last_row) as usize;
                if top < bottom {
                    self.scroll_top = top;
                    self.scroll_bottom = bottom;
                    self.move_to(0, 0);
                }
            }
            b's' => self.saved = self.save_cursor(),
            b'u' => self.move_to(self.saved.row as isize, self.saved.col as isize),
            b'h' | b'l' => {
                for &mode in params {
                    self.set_mode(mode, false, last == b'h');
                }
            }
            b'n' => match params.first().copied().unwrap_or(0) {
                5 => self.respond(b"\x1b[0n"),
                6 => {
                    let (row, col) = self.cursor();
                    let report = alloc::format!("\x1b[{};{}R", row + 1, col + 1);
                    self.respond(report.as_bytes());
                }
                _ => {}
            },
            b'c' => self.respond(b"\x1b[?6c"),
            _ => {}
        }
    }

    fn sgr(&mut self, params: &[u32]) {
        let params = if params.is_empty() { &[0][..] } else { params };
        let mut i = 0;
        while i < params.len() {
            let v = params[i];
            match v {
                0 => self.pen = Pen::DEFAULT,
                1 => self.pen.attr |= attr::BOLD,
                2 => self.pen.attr |= attr::DIM,
                4 => self.pen.attr |= attr::UNDERLINE,
                7 => self.pen.attr |= attr::REVERSE,
                22 => self.pen.attr &= !(attr::BOLD | attr::DIM),
                24 => self.pen.attr &= !attr::UNDERLINE,
                27 => self.pen.attr &= !attr::REVERSE,
                39 => self.pen.attr |= attr::FG_DEFAULT,
                49 => self.pen.attr |= attr::BG_DEFAULT,
                38 | 48 => {
                    let byte = |v: u32| v.min(255);
                    let index = match params.get(i + 1) {
                        Some(5) if i + 2 < params.len() => {
                            i += 2;
                            byte(params[i]) as u8
                        }
                        Some(2) if i + 4 < params.len() => {
                            let level = |v: u32| byte(v) * 5 / 255;
                            let index = 16 + 36 * level(params[i + 2]) + 6 * level(params[i + 3]) + level(params[i + 4]);
                            i += 4;
                            index as u8
                        }
                        _ => {
                            i += 1;
                            continue;
                        }
                    };
                    self.set_color(v == 38, index);
                }
                30..=37 => self.set_color(true, (v - 30) as u8),
                90..=97 => self.set_color(true, (v - 90 + 8) as u8),
                40..=47 => self.set_color(false, (v - 40) as u8),
                100..=107 => self.set_color(false, (v - 100 + 8) as u8),
                _ => {}
            }
            i += 1;
        }
    }

    fn set_color(&mut self, fg: bool, index: u8) {
        if fg {
            self.pen.fg = index;
            self.pen.attr &= !attr::FG_DEFAULT;
        } else {
            self.pen.bg = index;
            self.pen.attr &= !attr::BG_DEFAULT;
        }
    }

    fn set_mode(&mut self, mode: u32, private: bool, on: bool) {
        if !private {
            if mode == 4 {
                self.insert = on;
            }
            return;
        }
        match mode {
            1 => self.app_cursor = on,
            7 => {
                self.autowrap = on;
                if !on {
                    self.wrap_pending = false;
                }
            }
            25 => self.cursor_visible = on,
            47 | 1047 | 1049 if on => self.enter_alt_screen(mode == 1049),
            47 | 1047 | 1049 => self.leave_alt_screen(mode == 1049),
            _ => {}
        }
    }

    fn enter_alt_screen(&mut self, save_cursor: bool) {
        if self.alt_active {
            return;
        }
        if save_cursor {
            self.alt_saved = self.save_cursor();
        }
        self.alt_active = true;
        self.erase_rows(0..self.rows);
        self.wrap_pending = false;
        self.mark_all_dirty();
    }

    fn leave_alt_screen(&mut self, restore_cursor: bool) {
        if !self.alt_active {
            return;
        }
        self.alt_active = false;
        if restore_cursor {
            self.restore_cursor(self.alt_saved);
        }
        self.wrap_pending = false;
        self.mark_all_dirty();
    }

    fn respond(&mut self, bytes: &[u8]) {
        self.response.extend_from_slice(bytes);
    }
}
