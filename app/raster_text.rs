/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * A line of text the agent drew (TEXT_IMAGE), cut short with an ellipsis.
 */

use alloc::vec::Vec;
use core::alloc::{AllocError, Allocator};

use agent_link::TextImage;
use devkit_gfx::{Color, FontChain, ImageBuf, ImageFormat, Rect, TextRun};
use devkit_ui::Canvas;

pub struct RasterText<A: Allocator> {
    ascent: u16,
    breaks: Vec<u16, A>,
    alpha: Option<ImageBuf<A>>,
}

impl<A: Allocator + Clone> RasterText<A> {
    pub fn copy_in(text: &TextImage<'_>, alloc: A) -> Result<Self, AllocError> {
        let mut breaks = Vec::new_in(alloc.clone());
        breaks.try_reserve_exact(text.breaks().count()).map_err(|_| AllocError)?;
        breaks.extend(text.breaks());
        let alpha = if text.width == 0 || text.height == 0 {
            None
        } else {
            let mut image = unsafe { ImageBuf::new_uninit_in(ImageFormat::A8, text.width, text.height, usize::from(text.width), 4, alloc)? };
            image.pixels_mut().copy_from_slice(text.alpha);
            Some(image)
        };
        Ok(Self { ascent: text.ascent, breaks, alpha })
    }
}

impl<A: Allocator> RasterText<A> {
    pub fn width(&self) -> i16 {
        self.alpha.as_ref().map_or(0, |alpha| alpha.width() as i16)
    }

    pub fn height(&self) -> i16 {
        self.alpha.as_ref().map_or(0, |alpha| alpha.height() as i16)
    }

    /// Where its top goes for its baseline to sit on that of a line box of `font`.
    pub fn top(&self, font: &FontChain) -> i16 {
        font.ascent() - self.ascent as i16
    }

    /// Whole, its top left at `x`, `y`.
    pub fn draw_at<'a>(&'a self, canvas: &mut Canvas<'a, '_>, x: i16, y: i16, color: Color) {
        if let Some(alpha) = &self.alpha {
            canvas.image(x, y, alpha.image(), color);
        }
    }

    /// On the line box of `font` at the canvas's top, centered when `center`
    /// and it fits; what does not fit its width ends at a character, followed
    /// by `ellipsis`.
    pub fn draw<'a>(&'a self, canvas: &mut Canvas<'a, '_>, font: &FontChain, ellipsis: &'a TextRun, color: Color, center: bool) {
        let (width, height) = canvas.size();
        if self.width() <= width {
            let x = if center { (width - self.width()) / 2 } else { 0 };
            self.draw_at(canvas, x, self.top(font), color);
            return;
        }
        let room = width - ellipsis.width();
        let cut = self.breaks.iter().rev().map(|&x| x as i16).find(|&x| x <= room).unwrap_or(0);
        canvas.push_clip(Rect::new(0, 0, cut, height));
        self.draw_at(canvas, 0, self.top(font), color);
        canvas.pop_clip();
        canvas.text(cut, 0, ellipsis, color);
    }
}
