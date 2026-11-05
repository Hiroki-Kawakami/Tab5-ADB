/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use devkit_gfx::FontChain;
use devkit_ui::Icon;

use crate::resources::{HERO_56, ICON_28, ICON_40};

pub static HERO: FontChain = FontChain::new(&[&HERO_56]);
static SMALL_ICONS: FontChain = FontChain::new(&[&ICON_28]);
static ICONS: FontChain = FontChain::new(&[&ICON_40]);

pub const fn small_icon(glyph: char) -> Icon {
    Icon { font: &SMALL_ICONS, glyph }
}

pub const fn icon(glyph: char) -> Icon {
    Icon { font: &ICONS, glyph }
}
