/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::sync::Arc;
use core::ffi::CStr;

use devkit_gfx::{Color, ImageSource};
use devkit_image::QrOptions;
use devkit_ui::widgets::{button, modal, modal_actions, modal_title};
use devkit_ui::{Align, Component, Cx, Element, Justify, Overlay, Screen, Ui};
use idf::log::error;

const TAG: &CStr = c"github";
const URL: &str = "https://github.com/Hiroki-Kawakami/Tab5-ADB";
const QR_SCALE: u16 = 8;
const QR_RADIUS: u8 = 16;

pub struct GitHubScreen {
    qr: Option<Arc<dyn ImageSource>>,
}

impl GitHubScreen {
    pub fn new() -> Self {
        let options = QrOptions { scale: QR_SCALE, ..QrOptions::default() };
        let qr = devkit_image::qr_image(URL.as_bytes(), &options).inspect_err(|err| error!(TAG, "qr: {err}")).ok();
        Self { qr: qr.map(|image| Arc::new(image) as Arc<dyn ImageSource>) }
    }
}

#[derive(Clone, Copy)]
pub struct Close;

impl Component for GitHubScreen {
    type Msg = Close;

    fn update(&mut self, _: Close, cx: &mut Cx<Self>) {
        cx.nav_of::<Overlay>().pop();
    }

    fn view<'a>(&'a self, ui: &'a Ui<Close>) -> Element<'a, Close> {
        let url = ui.text(ui.theme().fonts.body, URL).wrap();
        let body = match &self.qr {
            Some(qr) => ui.column([ui.row([ui.image(qr)]).radius(QR_RADIUS).background(Color::hex(0xffffff)), url]).gap(20),
            None => ui.column([url]),
        };
        modal(ui, [
            modal_title(ui, "GitHub"),
            body.align(Align::Center).justify(Justify::Center),
            modal_actions(ui, [button(ui, "Close", ui.theme().widgets.button.primary, Close)]),
        ])
    }
}

impl Screen for GitHubScreen {
    fn scrim_tap(&self) -> Option<Close> {
        Some(Close)
    }
}
