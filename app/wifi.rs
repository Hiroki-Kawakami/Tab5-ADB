/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * The Tab5's own Wi-Fi: started at boot and joined by the Wi-Fi manager.
 */

use alloc::string::String;
use core::ffi::CStr;
use core::time::Duration;

use devkit_net::wifi_manager::{self, JoinError, Manager, State};
use devkit_ui::{Component, This};
use idf::executor;
use idf::log::error;

use crate::device_info;
use crate::resources::icons;

const TAG: &CStr = c"wifi";
const IPV4_POLL: Duration = Duration::from_secs(1);

pub fn start() {
    if let Err(err) = devkit_net::start(devkit_net::Config::default()) {
        error!(TAG, "devkit_net::start: {err}");
        return;
    }
    if let Err(err) = wifi_manager::start() {
        error!(TAG, "wifi_manager::start: {err}");
    }
}

pub fn manager() -> Option<&'static Manager> {
    wifi_manager::get()
}

pub fn state() -> State {
    manager().map_or(State::Disabled, Manager::state)
}

pub fn ssid(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).into_owned()
}

/// The `wifi` glyph showing as many arcs as `rssi` is worth; full while the
/// strength is not known yet.
pub fn strength_glyph(rssi: Option<i8>) -> char {
    match rssi.map_or(4, |rssi| device_info::Wifi::bars(i32::from(rssi))) {
        4 => icons::WIFI,
        3 => icons::WIFI_2,
        2 => icons::WIFI_1,
        _ => icons::WIFI_0,
    }
}

pub fn ipv4() -> Option<String> {
    devkit_net::wifi()?.sta().ipv4().map(|config| alloc::format!("{}", config.address))
}

pub fn mac() -> Option<String> {
    let mac = devkit_net::wifi()?.sta().link_info().mac;
    Some(alloc::format!("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]))
}

pub fn join_error(err: JoinError) -> String {
    match err {
        JoinError::Disabled => "Wi-Fi is off.".into(),
        JoinError::InvalidSsid => "The network name must be 1 to 32 bytes.".into(),
        JoinError::InvalidPassword => "The password must be 8 to 63 characters.".into(),
        JoinError::NotSaved => "The network is not saved.".into(),
        JoinError::NotFound => "The network was not found.".into(),
        JoinError::AuthFailed => "Incorrect password.".into(),
        JoinError::Rejected(reason) => alloc::format!("The network refused the connection ({reason:?})."),
        JoinError::Timeout => "The connection timed out.".into(),
        JoinError::Driver(code) => alloc::format!("Wi-Fi error {code}."),
    }
}

/// Calls `changed` and rebuilds whenever the Wi-Fi state changes, and every
/// second while connected without an address yet.
pub async fn follow<C: Component>(this: This<C>, changed: fn(&mut C)) {
    let Some(manager) = manager() else { return };
    loop {
        if this.with(|c, _| changed(c)).is_none() {
            return;
        }
        if matches!(manager.state(), State::Connected { .. }) && ipv4().is_none() {
            let _ = executor::timeout(IPV4_POLL, manager.changed()).await;
        } else {
            manager.changed().await;
        }
    }
}
