/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use devkit_ui::widgets::top_bar;
use devkit_ui::{Component, Cx, Element, Screen, Ui};

pub struct Home;

impl Component for Home {
    type Msg = ();

    fn update(&mut self, _: (), _: &mut Cx<Self>) {}

    fn view<'a>(&'a self, ui: &'a Ui<()>) -> Element<'a, ()> {
        ui.column([top_bar(ui, None, "Tab5-ADB", ui.empty())])
            .padding_each(0, ui.insets().top, 0, 0)
            .background(ui.colors().background)
    }
}

impl Screen for Home {}
