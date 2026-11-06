/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::ToString;

use adb::link::{Connection, State};
use devkit_ui::widgets::{button, modal, modal_actions, modal_message, modal_title, progress_indicator};
use devkit_ui::{Align, Component, Cx, Element, Overlay, Root, Screen, Task, This, Transition, Ui};

use super::adb_dashboard::AdbDashboardScreen;
use super::message::MessageScreen;

pub struct ConnectingScreen {
    conn: Connection,
    state: State,
    task: Option<Task>,
}

impl ConnectingScreen {
    pub fn new(conn: Connection) -> Self {
        let state = conn.state();
        Self { conn, state, task: None }
    }
}

#[derive(Clone, Copy)]
pub struct Cancel;

async fn follow(this: This<ConnectingScreen>, conn: Connection) {
    loop {
        let state = conn.state();
        match state {
            State::Online => {
                this.with(|_, cx| {
                    cx.nav_of::<Overlay>().pop();
                    cx.nav_of::<Root>().transition(Transition::Zoom).push(AdbDashboardScreen::new(conn.clone()));
                });
                return;
            }
            State::Closed(err) => {
                this.with(|_, cx| cx.nav_of::<Overlay>().push(MessageScreen::new("Connection failed", err.to_string())));
                return;
            }
            _ => {
                let alive = this.with(|s, cx| {
                    s.state = state;
                    cx.rebuild();
                });
                if alive.is_none() {
                    return;
                }
            }
        }
        conn.state_changed().await;
    }
}

impl Component for ConnectingScreen {
    type Msg = Cancel;

    fn update(&mut self, _: Cancel, cx: &mut Cx<Self>) {
        if let Some(task) = self.task.take() {
            task.abort();
        }
        cx.nav_of::<Overlay>().pop();
    }

    fn view<'a>(&'a self, ui: &'a Ui<Cancel>) -> Element<'a, Cancel> {
        let theme = ui.theme();
        let message = match self.state {
            State::Unauthorized => "Allow USB debugging on the phone.",
            _ => "Waiting for the device...",
        };
        modal(ui, [
            modal_title(ui, "Connecting"),
            ui.row([progress_indicator(ui, None), modal_message(ui, message).grow(1)]).align(Align::Center).gap(theme.widgets.unit.padding),
            modal_actions(ui, [button(ui, "Cancel", theme.widgets.button.secondary, Cancel)]),
        ])
    }
}

impl Screen for ConnectingScreen {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        let conn = self.conn.clone();
        self.task = Some(cx.spawn(|this| follow(this, conn)));
    }
}
