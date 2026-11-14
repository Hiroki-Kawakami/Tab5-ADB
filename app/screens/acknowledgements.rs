/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::vec::Vec;

use devkit_ui::widgets::top_bar;
use devkit_ui::{Component, Cx, Element, Screen, Ui, id};

const TEXT: &str = include_str!("../third_party_licenses.txt");
const MARGIN: i16 = 24;
const GAP: i16 = 16;

pub struct AcknowledgementsScreen {
    paragraphs: Vec<&'static str>,
}

impl AcknowledgementsScreen {
    pub fn new() -> Self {
        Self { paragraphs: TEXT.split("\n\n").map(str::trim).filter(|p| !p.is_empty()).collect() }
    }
}

#[derive(Clone, Copy)]
pub struct Back;

impl Component for AcknowledgementsScreen {
    type Msg = Back;

    fn update(&mut self, _: Back, cx: &mut Cx<Self>) {
        cx.nav().pop();
    }

    fn view<'a>(&'a self, ui: &'a Ui<Back>) -> Element<'a, Back> {
        let caption = ui.theme().fonts.caption;
        let paragraphs: Vec<_> = self.paragraphs.iter().map(|p| ui.text(caption, p).wrap()).collect();
        let body = ui.column(paragraphs).gap(GAP).padding_each(MARGIN, 0, MARGIN, MARGIN + ui.insets().bottom);
        ui.column([top_bar(ui, Some(Back), "Acknowledgements", ui.empty()).scroll_border(id!("content")), ui.scroll(body).grow(1).id(id!("content"))])
            .padding_each(0, ui.insets().top, 0, 0)
            .background(ui.colors().background)
    }
}

impl Screen for AcknowledgementsScreen {}
