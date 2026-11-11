/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::String;
use core::ffi::CStr;

use adb::link::{Connection, State};
use devkit_ui::widgets::{button, modal, modal_actions, modal_message, modal_title, progress_indicator};
use devkit_ui::{Align, Component, Cx, Element, Overlay, Root, Screen, Task, This, Transition, Ui};
use idf::log::error;

use crate::adb_link::{self, Route};
use crate::agent::Agent;
use crate::settings::{self, AgentMode};

use super::adb_dashboard::AdbDashboardScreen;
use super::message::MessageScreen;

const TAG: &CStr = c"connecting";

pub struct ConnectingScreen {
    route: Route,
    conn: Option<Connection>,
    state: State,
    starting_agent: bool,
    task: Option<Task>,
}

impl ConnectingScreen {
    pub fn usb(conn: Connection) -> Self {
        let state = conn.state();
        Self { route: Route::Usb, conn: Some(conn), state, starting_agent: false, task: None }
    }

    /// Opens the connection to `addr` itself.
    pub fn tcp(addr: String) -> Self {
        Self { route: Route::Tcp(addr), conn: None, state: State::Connecting, starting_agent: false, task: None }
    }
}

#[derive(Clone, Copy)]
pub struct Cancel;

async fn open(this: This<ConnectingScreen>, route: Route, conn: Option<Connection>) {
    let conn = match (conn, &route) {
        (Some(conn), _) => conn,
        (None, Route::Tcp(addr)) => match adb_link::connect_tcp(addr).await {
            Ok(conn) => conn,
            Err(err) => {
                this.with(|_, cx| cx.nav_of::<Overlay>().push(MessageScreen::new("Connection failed", err)));
                return;
            }
        },
        (None, Route::Usb) => return,
    };
    follow(this, route, conn).await;
}

async fn follow(this: This<ConnectingScreen>, route: Route, conn: Connection) {
    loop {
        let state = conn.state();
        match state {
            State::Online => {
                open_dashboard(this, route, conn).await;
                return;
            }
            State::Closed(err) => {
                let message = adb_link::describe(&route, &err);
                this.with(|_, cx| cx.nav_of::<Overlay>().push(MessageScreen::new("Connection failed", message)));
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

async fn open_dashboard(this: This<ConnectingScreen>, route: Route, conn: Connection) {
    let agent = if settings::agent_mode() == AgentMode::Normal {
        let alive = this.with(|s, cx| {
            s.starting_agent = true;
            cx.rebuild();
        });
        if alive.is_none() {
            return;
        }
        Agent::start(conn.clone()).await.inspect_err(|err| error!(TAG, "agent: {err}")).ok()
    } else {
        None
    };
    if let State::Closed(err) = conn.state() {
        let message = adb_link::describe(&route, &err);
        this.with(|_, cx| cx.nav_of::<Overlay>().push(MessageScreen::new("Connection failed", message)));
        return;
    }
    this.with(|_, cx| {
        cx.nav_of::<Overlay>().pop();
        cx.nav_of::<Root>().transition(Transition::Zoom).push(AdbDashboardScreen::new(conn, route, agent));
    });
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
            _ if self.starting_agent => "Starting the agent...",
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
        let (route, conn) = (self.route.clone(), self.conn.clone());
        self.task = Some(cx.spawn(|this| open(this, route, conn)));
    }
}
