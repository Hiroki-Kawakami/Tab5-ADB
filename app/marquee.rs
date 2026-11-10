/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * A line of text that scrolls sideways when it is wider than its canvas.
 */

use core::cell::Cell;
use core::time::Duration;

use devkit_gfx::Rect;
use devkit_ui::Canvas;
use idf::time::Instant;

const PAUSE_MS: u64 = 2000;
const SPEED: u64 = 40;
const GAP: i16 = 64;
/// How often a moving marquee needs drawing.
pub const FRAME: Duration = Duration::from_millis(33);

pub struct Marquee {
    start: Cell<Instant>,
    moving: Cell<bool>,
}

impl Marquee {
    pub fn new() -> Self {
        Self { start: Cell::new(Instant::now()), moving: Cell::new(false) }
    }

    /// Starts over from the pause at the left, as for new text.
    pub fn restart(&self) {
        self.start.set(Instant::now());
    }

    /// The last draw did not fit, so the canvas needs drawing every `FRAME`.
    pub fn moving(&self) -> bool {
        self.moving.get()
    }

    /// `paint` draws the text, `width` wide, at an x on the canvas; centered
    /// when `center` and it fits.
    pub fn draw<'a, 'f>(&self, canvas: &mut Canvas<'a, 'f>, width: i16, center: bool, mut paint: impl FnMut(&mut Canvas<'a, 'f>, i16)) {
        let (room, height) = canvas.size();
        if width <= room {
            self.moving.set(false);
            paint(canvas, if center { (room - width) / 2 } else { 0 });
            return;
        }
        self.moving.set(true);
        let lap = (width + GAP) as u64;
        let period = PAUSE_MS + lap * 1000 / SPEED;
        let at = self.start.get().elapsed().as_millis() as u64 % period;
        let offset = at.saturating_sub(PAUSE_MS) * SPEED / 1000;
        canvas.push_clip(Rect::new(0, 0, room, height));
        paint(canvas, -(offset as i16));
        paint(canvas, (lap - offset) as i16);
        canvas.pop_clip();
    }
}
