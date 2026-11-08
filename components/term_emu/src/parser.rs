/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Bytes to dispatches: a reduced Paul Williams VT500 parser.
 */

use alloc::alloc::Allocator;

use crate::Terminal;

const BEL: u8 = 0x07;
const CAN: u8 = 0x18;
const SUB: u8 = 0x1a;
const ESC: u8 = 0x1b;
const DEL: u8 = 0x7f;
const MAX_PARAMS: usize = 16;
const MAX_PARAM: u32 = 100_000;

#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    Ground,
    Escape,
    EscapeIntermediate,
    Csi,
    /// OSC: ends at BEL or ST.
    Osc,
    OscEscape,
    /// DCS, SOS, PM and APC: ends at ST.
    Ignored,
    IgnoredEscape,
}

pub(crate) struct Parser {
    state: State,
    params: [u32; MAX_PARAMS],
    count: usize,
    param_seen: bool,
    ignore: bool,
    private: u8,
    intermediate: u8,
    utf8: u32,
    utf8_left: u8,
}

impl Parser {
    pub(crate) const fn new() -> Self {
        Self { state: State::Ground, params: [0; MAX_PARAMS], count: 0, param_seen: false, ignore: false, private: 0, intermediate: 0, utf8: 0, utf8_left: 0 }
    }

    fn to_ground(&mut self) {
        let (utf8, utf8_left) = (self.utf8, self.utf8_left);
        *self = Self { utf8, utf8_left, ..Self::new() };
    }
}

impl<A: Allocator + Clone> Terminal<A> {
    /// Output of the host. Chunks may split anywhere, sequences included.
    pub fn feed(&mut self, data: &[u8]) {
        for &b in data {
            self.parse(b);
        }
    }

    fn parse(&mut self, b: u8) {
        match self.parser.state {
            State::Osc | State::Ignored => {
                match b {
                    BEL if self.parser.state == State::Osc => self.parser.to_ground(),
                    ESC => self.parser.state = if self.parser.state == State::Osc { State::OscEscape } else { State::IgnoredEscape },
                    CAN | SUB => self.parser.to_ground(),
                    _ => {}
                }
                return;
            }
            State::OscEscape | State::IgnoredEscape => {
                self.parser.to_ground();
                if b != b'\\' {
                    self.parser.state = State::Escape;
                    self.parse(b);
                }
                return;
            }
            _ => {}
        }
        if b < 0x20 {
            self.parser.utf8_left = 0;
            match b {
                ESC => {
                    self.parser.to_ground();
                    self.parser.state = State::Escape;
                }
                CAN | SUB => self.parser.to_ground(),
                _ => self.execute(b),
            }
            return;
        }
        match self.parser.state {
            State::Ground => self.ground(b),
            State::Escape => match b {
                0x20..=0x2f => {
                    self.parser.intermediate = b;
                    self.parser.state = State::EscapeIntermediate;
                }
                b'[' => self.parser.state = State::Csi,
                b']' => self.parser.state = State::Osc,
                b'P' | b'X' | b'^' | b'_' => self.parser.state = State::Ignored,
                _ => {
                    self.parser.to_ground();
                    if (0x30..=0x7e).contains(&b) {
                        self.esc_dispatch(0, b);
                    }
                }
            },
            State::EscapeIntermediate => match b {
                0x20..=0x2f => self.parser.intermediate = b,
                0x30..=0x7e => {
                    let intermediate = self.parser.intermediate;
                    self.parser.to_ground();
                    self.esc_dispatch(intermediate, b);
                }
                _ => self.parser.to_ground(),
            },
            State::Csi => self.csi(b),
            _ => {}
        }
    }

    fn ground(&mut self, b: u8) {
        let p = &mut self.parser;
        if p.utf8_left > 0 {
            if b & 0xc0 == 0x80 {
                p.utf8 = (p.utf8 << 6) | u32::from(b & 0x3f);
                p.utf8_left -= 1;
                if p.utf8_left == 0 {
                    let cp = p.utf8;
                    self.print(cp);
                }
            } else {
                p.utf8_left = 0;
                self.print(0xfffd);
                self.parse(b);
            }
            return;
        }
        match b {
            DEL => {}
            0x00..=0x7f => self.print(u32::from(b)),
            _ if b & 0xe0 == 0xc0 => (p.utf8, p.utf8_left) = (u32::from(b & 0x1f), 1),
            _ if b & 0xf0 == 0xe0 => (p.utf8, p.utf8_left) = (u32::from(b & 0x0f), 2),
            _ if b & 0xf8 == 0xf0 => (p.utf8, p.utf8_left) = (u32::from(b & 0x07), 3),
            _ => self.print(0xfffd),
        }
    }

    fn csi(&mut self, b: u8) {
        let p = &mut self.parser;
        match b {
            b'0'..=b'9' => {
                let param = &mut p.params[p.count];
                if *param < MAX_PARAM {
                    *param = *param * 10 + u32::from(b - b'0');
                }
                p.param_seen = true;
            }
            b';' | b':' => {
                if p.count < MAX_PARAMS - 1 {
                    p.count += 1;
                    p.params[p.count] = 0;
                } else {
                    p.ignore = true;
                }
                p.param_seen = true;
            }
            0x3c..=0x3f => {
                if p.count == 0 && !p.param_seen && p.private == 0 {
                    p.private = b;
                } else {
                    p.ignore = true;
                }
            }
            0x20..=0x2f => p.intermediate = b,
            0x40..=0x7e => {
                let (private, intermediate, params, count) = (p.private, p.intermediate, p.params, if p.param_seen { p.count + 1 } else { 0 });
                let ignore = p.ignore;
                p.to_ground();
                if !ignore {
                    self.csi_dispatch(private, &params[..count], intermediate, b);
                }
            }
            _ => p.ignore = true,
        }
    }
}
