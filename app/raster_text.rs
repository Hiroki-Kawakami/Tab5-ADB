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
    /// On the line box of `font` at the canvas's top left; what does not fit
    /// its width ends at a character, followed by `ellipsis`.
    pub fn draw<'a>(&'a self, canvas: &mut Canvas<'a, '_>, font: &FontChain, ellipsis: &'a TextRun, color: Color) {
        let Some(alpha) = &self.alpha else { return };
        let (width, height) = canvas.size();
        let y = font.ascent() - self.ascent as i16;
        if alpha.width() as i16 <= width {
            canvas.image(0, y, alpha.image(), color);
            return;
        }
        let room = width - ellipsis.width();
        let cut = self.breaks.iter().rev().map(|&x| x as i16).find(|&x| x <= room).unwrap_or(0);
        canvas.push_clip(Rect::new(0, 0, cut, height));
        canvas.image(0, y, alpha.image(), color);
        canvas.pop_clip();
        canvas.text(cut, 0, ellipsis, color);
    }
}
