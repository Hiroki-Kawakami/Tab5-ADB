/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::String;

use devkit_ui::widgets::{button, modal, modal_actions, modal_message, modal_title};
use devkit_ui::{Component, Cx, Element, Overlay, Screen, Ui};

pub struct MessageScreen {
    title: &'static str,
    text: String,
}

impl MessageScreen {
    pub fn new(title: &'static str, text: impl Into<String>) -> Self {
        Self { title, text: text.into() }
    }
}

#[derive(Clone, Copy)]
pub struct Close;

impl Component for MessageScreen {
    type Msg = Close;

    fn update(&mut self, _: Close, cx: &mut Cx<Self>) {
        cx.nav_of::<Overlay>().pop();
    }

    fn view<'a>(&'a self, ui: &'a Ui<Close>) -> Element<'a, Close> {
        modal(ui, [
            modal_title(ui, self.title),
            modal_message(ui, &self.text),
            modal_actions(ui, [button(ui, "OK", ui.theme().widgets.button.primary, Close)]),
        ])
    }
}

impl Screen for MessageScreen {
    fn scrim_tap(&self) -> Option<Close> {
        Some(Close)
    }
}
