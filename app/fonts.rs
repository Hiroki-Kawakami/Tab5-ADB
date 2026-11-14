/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use devkit_gfx::FontChain;
use devkit_ui::Icon;

use crate::resources::{HERO_56, ICON_28, ICON_36, ICON_40, ICON_48, ICON_72, ICON_120, JP_18, MONO_18};

pub static HERO: FontChain = FontChain::new(&[&HERO_56]);
pub static MONO: FontChain = FontChain::new(&[&MONO_18, &JP_18]);
static SMALL_ICONS: FontChain = FontChain::new(&[&ICON_28]);
static ICONS: FontChain = FontChain::new(&[&ICON_40]);
static ICONS_36: FontChain = FontChain::new(&[&ICON_36]);
static ICONS_48: FontChain = FontChain::new(&[&ICON_48]);
static ICONS_72: FontChain = FontChain::new(&[&ICON_72]);
static ICONS_120: FontChain = FontChain::new(&[&ICON_120]);

pub const fn small_icon(glyph: char) -> Icon {
    Icon { font: &SMALL_ICONS, glyph }
}

pub const fn icon(glyph: char) -> Icon {
    Icon { font: &ICONS, glyph }
}

pub const fn icon_36(glyph: char) -> Icon {
    Icon { font: &ICONS_36, glyph }
}

pub const fn icon_48(glyph: char) -> Icon {
    Icon { font: &ICONS_48, glyph }
}

pub const fn icon_72(glyph: char) -> Icon {
    Icon { font: &ICONS_72, glyph }
}

pub const fn icon_120(glyph: char) -> Icon {
    Icon { font: &ICONS_120, glyph }
}
