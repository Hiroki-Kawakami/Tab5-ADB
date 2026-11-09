/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::{String, ToString};
use alloc::vec::Vec;

use adb::link::Connection;

use crate::files::shell_quote;

const SEPARATOR: &str = "---SEP---";

pub const LIST_COMMAND: &str = "pm list packages -3; echo ---SEP---; pm list packages -s; echo ---SEP---; pm list packages -d";

#[derive(Clone, PartialEq, Eq, Debug)]
pub struct App {
    pub package: String,
    pub system: bool,
    pub disabled: bool,
}

#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct Details {
    pub version: String,
    pub installed: String,
    pub updated: String,
    pub path: String,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Action {
    Launch,
    ForceStop,
    ClearData,
    Disable,
    Enable,
    Uninstall,
}

impl Action {
    pub fn command(self, package: &str) -> String {
        let package = shell_quote(package);
        match self {
            Action::Launch => alloc::format!("monkey -p {package} -c android.intent.category.LAUNCHER 1 2>&1"),
            Action::ForceStop => alloc::format!("am force-stop {package} 2>&1"),
            Action::ClearData => alloc::format!("pm clear {package} 2>&1"),
            Action::Disable => alloc::format!("pm disable-user --user 0 {package} 2>&1"),
            Action::Enable => alloc::format!("pm enable {package} 2>&1"),
            Action::Uninstall => alloc::format!("pm uninstall {package} 2>&1"),
        }
    }

    pub fn succeeded(self, out: &str) -> bool {
        let out = out.trim();
        match self {
            Action::Launch => out.contains("Events injected"),
            Action::ForceStop => out.is_empty(),
            Action::ClearData | Action::Uninstall => out.lines().any(|line| line.trim() == "Success"),
            Action::Disable => out.contains("new state: disabled"),
            Action::Enable => out.contains("new state: enabled"),
        }
    }

    pub fn failure(self) -> &'static str {
        match self {
            Action::Launch => "Launch failed",
            Action::ForceStop => "Force stop failed",
            Action::ClearData => "Clear data failed",
            Action::Disable => "Disable failed",
            Action::Enable => "Enable failed",
            Action::Uninstall => "Uninstall failed",
        }
    }
}

/// Sorted by package name.
pub fn parse_list(out: &str) -> Vec<App> {
    let mut sections = out.split(SEPARATOR);
    let user = packages(sections.next().unwrap_or(""));
    let system = packages(sections.next().unwrap_or(""));
    let disabled = packages(sections.next().unwrap_or(""));
    let app = |package: &str, system: bool| App { package: package.to_string(), system, disabled: disabled.contains(&package) };
    let mut apps: Vec<App> = user.iter().map(|p| app(p, false)).collect();
    apps.extend(system.iter().filter(|p| !user.contains(p)).map(|p| app(p, true)));
    apps.sort_by_cached_key(|a| a.package.to_ascii_lowercase());
    apps
}

fn packages(section: &str) -> Vec<&str> {
    section.lines().filter_map(|line| line.trim().strip_prefix("package:")).filter(|p| !p.is_empty()).collect()
}

pub fn details_command(package: &str) -> String {
    let package = shell_quote(package);
    alloc::format!("dumpsys package {package}; echo ---SEP---; pm path {package}")
}

pub fn parse_details(out: &str, package: &str) -> Details {
    let (dump, paths) = out.split_once(SEPARATOR).unwrap_or((out, ""));
    let header = alloc::format!("Package [{package}]");
    let dump = dump.find(&header).map_or(dump, |at| &dump[at..]);
    Details {
        version: value(dump, "versionName").to_string(),
        installed: time(value(dump, "firstInstallTime")).to_string(),
        updated: time(value(dump, "lastUpdateTime")).to_string(),
        path: paths.lines().find_map(|line| line.trim().strip_prefix("package:")).unwrap_or("").to_string(),
    }
}

fn value<'a>(dump: &'a str, key: &str) -> &'a str {
    dump.lines().find_map(|line| line.trim().strip_prefix(key)?.strip_prefix('=')).map_or("", str::trim)
}

fn time(stamp: &str) -> &str {
    match stamp.rfind(':') {
        Some(at) if stamp[..at].contains(':') => &stamp[..at],
        _ => stamp,
    }
}

pub async fn list(conn: &Connection) -> Result<Vec<App>, String> {
    let out = adb::services::exec(conn, LIST_COMMAND).await.map_err(|err| err.to_string())?;
    Ok(parse_list(&String::from_utf8_lossy(&out)))
}

pub async fn details(conn: &Connection, package: &str) -> Result<Details, String> {
    let out = adb::services::exec(conn, &details_command(package)).await.map_err(|err| err.to_string())?;
    Ok(parse_details(&String::from_utf8_lossy(&out), package))
}

pub async fn run(conn: &Connection, action: Action, package: &str) -> Result<(), String> {
    let out = adb::services::exec(conn, &action.command(package)).await.map_err(|err| err.to_string())?;
    let out = String::from_utf8_lossy(&out);
    if action.succeeded(&out) {
        return Ok(());
    }
    Err(failure_reason(&out).to_string())
}

pub fn failure_reason(out: &str) -> &str {
    let lines = || out.lines().map(str::trim).filter(|line| !line.is_empty() && !line.starts_with("bash arg:"));
    lines().find(|line| line.starts_with("Failure") || line.starts_with("Error") || line.starts_with("**")).or_else(|| lines().next()).unwrap_or("The command failed.")
}
