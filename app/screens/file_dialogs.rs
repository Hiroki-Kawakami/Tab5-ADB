/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::{String, ToString};
use alloc::vec::Vec;
use core::ffi::CStr;
use core::time::Duration;

use devkit_ui::widgets::{button, modal, modal_actions, modal_message, modal_title, progress_bar, text_field};
use devkit_ui::{Component, Cx, Element, Handle, Overlay, Screen, TextBuffer, TextInput, This, Ui, id};
use idf::log::error;

use crate::file_ops::{Operation, Progress};
use crate::files::{self, Volume};
use crate::resources::colors;

use super::file_manager::{Msg, changed};
use super::message::MessageScreen;

const TAG: &CStr = c"file_dialogs";
const PROGRESS_POLL: Duration = Duration::from_millis(100);

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Action {
    Secondary,
    Primary,
    Destructive,
}

/// A question answered by one of its buttons; the first one is also what a
/// tap on the scrim picks.
pub struct Choice {
    title: &'static str,
    message: String,
    buttons: Vec<(&'static str, Action, Option<Msg>)>,
    target: Handle<Msg>,
}

impl Choice {
    pub fn new(title: &'static str, message: String, buttons: Vec<(&'static str, Action, Option<Msg>)>, target: Handle<Msg>) -> Self {
        Self { title, message, buttons, target }
    }
}

#[derive(Clone, Copy)]
pub struct Pick(u8);

impl Component for Choice {
    type Msg = Pick;

    fn update(&mut self, Pick(index): Pick, cx: &mut Cx<Self>) {
        cx.nav_of::<Overlay>().pop();
        // Not send_to: that runs the answer before this pop, which would then
        // close whatever the answer opens on the overlay.
        if let Some(&(_, _, Some(msg))) = self.buttons.get(usize::from(index))
            && !self.target.post(msg)
        {
            error!(TAG, "choice: UI queue full");
        }
    }

    fn view<'a>(&'a self, ui: &'a Ui<Pick>) -> Element<'a, Pick> {
        let buttons = self.buttons.iter().enumerate().map(|(i, &(label, action, _))| button(ui, label, style(ui, action), Pick(i as u8)));
        modal(ui, [modal_title(ui, self.title), modal_message(ui, &self.message), modal_actions(ui, buttons)])
    }
}

impl Screen for Choice {
    fn scrim_tap(&self) -> Option<Pick> {
        Some(Pick(0))
    }
}

fn style<M: Copy>(ui: &Ui<M>, action: Action) -> &'static devkit_ui::StateStyle {
    let b = &ui.theme().widgets.button;
    match action {
        Action::Secondary => b.secondary,
        Action::Primary => b.primary,
        Action::Destructive => b.destructive,
    }
}

pub enum Naming {
    Rename { from: String, folder: bool },
    NewFolder,
}

/// Asks for a name in `dir` and renames or creates there.
pub struct NameDialog {
    naming: Naming,
    volume: Volume,
    dir: String,
    taken: Vec<String>,
    fold_case: bool,
    name: TextBuffer,
    error: Option<String>,
    busy: bool,
    target: Handle<Msg>,
}

impl NameDialog {
    /// `taken` are the names already in `dir`, compared without regard to
    /// case with `fold_case`.
    pub fn new(naming: Naming, volume: Volume, dir: String, taken: Vec<String>, fold_case: bool, target: Handle<Msg>) -> Self {
        let mut name = TextBuffer::new();
        if let Naming::Rename { from, folder } = &naming {
            name.set_text(from);
            if let Some(dot) = files::extension_at(from, *folder) {
                name.apply(TextInput::Place(dot as u32));
            }
        }
        Self { naming, volume, dir, taken, fold_case, name, error: None, busy: false, target }
    }

    fn same(&self, a: &str, b: &str) -> bool {
        if self.fold_case { a.eq_ignore_ascii_case(b) } else { a == b }
    }

    fn submit(&mut self, cx: &mut Cx<Self>) {
        if self.busy {
            return;
        }
        let name = self.name.as_str().trim().to_string();
        if let Err(err) = files::validate_name(&name) {
            self.error = Some(err.to_string());
            return;
        }
        let from = match &self.naming {
            Naming::Rename { from, .. } if *from == name => {
                cx.nav_of::<Overlay>().pop();
                return;
            }
            Naming::Rename { from, .. } => Some(from.clone()),
            Naming::NewFolder => None,
        };
        let only_case = from.as_deref().is_some_and(|from| from.eq_ignore_ascii_case(&name));
        if !only_case && self.taken.iter().any(|t| self.same(t, &name)) {
            self.error = Some(alloc::format!("\"{name}\" already exists."));
            return;
        }
        self.busy = true;
        self.error = None;
        let (volume, target) = (self.volume.clone(), files::join(&self.dir, &name));
        let from = from.map(|from| files::join(&self.dir, &from));
        cx.spawn(|this| async move {
            let result = match &from {
                Some(from) => volume.rename(from, &target).await,
                None => volume.create_dir(&target).await,
            };
            this.with(|s, cx| match result {
                Ok(()) => {
                    changed();
                    cx.send_to(&s.target, Msg::Reload);
                    cx.nav_of::<Overlay>().pop();
                }
                Err(err) => {
                    s.busy = false;
                    s.error = Some(err);
                    cx.rebuild();
                }
            });
        });
    }
}

#[derive(Clone, Copy)]
pub enum NameMsg {
    Input(TextInput),
    Cancel,
    Submit,
}

impl Component for NameDialog {
    type Msg = NameMsg;

    fn update(&mut self, msg: NameMsg, cx: &mut Cx<Self>) {
        match msg {
            NameMsg::Input(TextInput::Enter) | NameMsg::Submit => self.submit(cx),
            NameMsg::Input(input) => {
                self.name.apply(input);
                self.error = None;
            }
            NameMsg::Cancel => cx.nav_of::<Overlay>().pop(),
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<NameMsg>) -> Element<'a, NameMsg> {
        let b = &ui.theme().widgets.button;
        let (title, action) = match self.naming {
            Naming::Rename { .. } => ("Rename", "Rename"),
            Naming::NewFolder => ("New Folder", "Create"),
        };
        let error = match &self.error {
            Some(err) => modal_message(ui, err).foreground(colors::DESTRUCTIVE).id(id!("error")),
            None => ui.empty(),
        };
        modal(ui, [
            modal_title(ui, title),
            text_field(ui, &self.name, NameMsg::Input).placeholder("Name").id(id!("name")),
            error,
            modal_actions(ui, [
                button(ui, "Cancel", b.secondary, NameMsg::Cancel).id(id!("cancel")),
                button(ui, action, b.primary, NameMsg::Submit).disabled(self.busy).id(id!("submit")),
            ]),
        ])
    }
}

impl Screen for NameDialog {
    fn will_appear(&mut self, cx: &mut Cx<Self>) {
        cx.focus(id!("name"));
    }

    fn scrim_tap(&self) -> Option<NameMsg> {
        Some(NameMsg::Cancel)
    }
}

/// Follows an `Operation` until it ends, then tells `target` to reload.
pub struct ProgressDialog {
    title: &'static str,
    failure: &'static str,
    op: Operation,
    progress: Progress,
    cancelling: bool,
    target: Handle<Msg>,
}

impl ProgressDialog {
    pub fn new(title: &'static str, failure: &'static str, op: Operation, target: Handle<Msg>) -> Self {
        let progress = op.progress();
        Self { title, failure, op, progress, cancelling: false, target }
    }
}

async fn follow(this: This<ProgressDialog>) {
    loop {
        idf::executor::sleep(PROGRESS_POLL).await;
        let finished = this.with(|s, cx| {
            s.progress = s.op.progress();
            if !s.progress.finished {
                cx.rebuild();
                return false;
            }
            changed();
            cx.send_to(&s.target, Msg::Reload);
            match &s.progress.error {
                Some(err) => cx.nav_of::<Overlay>().push(MessageScreen::new(s.failure, err.clone())),
                None => cx.nav_of::<Overlay>().pop(),
            }
            true
        });
        if finished != Some(false) {
            return;
        }
    }
}

#[derive(Clone, Copy)]
pub struct CancelOp;

impl Component for ProgressDialog {
    type Msg = CancelOp;

    fn update(&mut self, _: CancelOp, cx: &mut Cx<Self>) {
        self.op.cancel();
        self.cancelling = true;
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<CancelOp>) -> Element<'a, CancelOp> {
        let p = &self.progress;
        let line = if p.total == 0 { ui.store(&p.current) } else { ui.format(format_args!("{} / {}  {}", (p.done + 1).min(p.total), p.total, p.current)) };
        modal(ui, [
            modal_title(ui, self.title),
            modal_message(ui, line).max_lines(1).ellipsis(devkit_ui::Elide::Middle),
            progress_bar(ui, p.fraction()),
            modal_actions(ui, [button(ui, "Cancel", ui.theme().widgets.button.secondary, CancelOp).disabled(self.cancelling).id(id!("cancel"))]),
        ])
    }
}

impl Screen for ProgressDialog {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        cx.spawn(follow);
    }
}
