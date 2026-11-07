/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * The ADB connection, to whichever device is plugged in over USB or to an
 * address over TCP; one at a time.
 */

use alloc::boxed::Box;
use alloc::format;
use alloc::string::{String, ToString};
use alloc::sync::Arc;
use core::ffi::CStr;

use adb::link::transport::Transport;
use adb::link::{Connection, Identity, State};
use devkit_net::{StaConfig, WifiState};
use idf::executor::Spawner;
use idf::log::{error, info};
use idf::sync::{Mutex, OnceLock};
use usb_host_rs::{Device, Event};
use idf::thread::Builder;

const TAG: &CStr = c"adb_link";
const KEY_NAMESPACE: &CStr = c"adb";
const DEFAULT_HOST_ID: &str = "00000000";
static SPAWNER: OnceLock<Spawner> = OnceLock::new();
static HOST_ID: OnceLock<String> = OnceLock::new();
static IDENTITY: OnceLock<Arc<Identity>> = OnceLock::new();
static DEVICE: Mutex<Option<Device>> = Mutex::new(None);
static CONNECTION: Mutex<Option<(Route, Connection)>> = Mutex::new(None);

#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Route {
    Usb,
    Tcp(String),
}

pub fn start(power: &bsp_rs::power::Power) {
    match power.set_switch(bsp_rs::power::Switch::Usb5V, true) {
        Ok(()) | Err(idf::Error::NOT_SUPPORTED) => {}
        Err(err) => error!(TAG, "USB 5V: {err}"),
    }
    let spawner = match Builder::new().name("adb").stack_size(16384).spawn_local_executor(|_| {}) {
        Ok(spawner) => spawner,
        Err(err) => {
            error!(TAG, "executor: {err}");
            return;
        }
    };
    let _ = SPAWNER.set(spawner.clone());
    if let Err(err) = usb_host_rs::start(&usb_host_rs::Config::default()) {
        error!(TAG, "usb_host_rs::start: {err}");
        return;
    }
    drop(spawner.spawn(follow()));
}

/// This Tab5's name towards devices: the low four bytes of the factory MAC.
pub fn host_id() -> &'static str {
    HOST_ID.get_or_init(|| match idf::system::factory_mac() {
        Ok(mac) => format!("{:02X}{:02X}{:02X}{:02X}", mac[2], mac[3], mac[4], mac[5]),
        Err(err) => {
            error!(TAG, "factory MAC: {err}");
            DEFAULT_HOST_ID.into()
        }
    })
}

/// The executor the connection's tasks run on.
pub fn spawner() -> Option<&'static Spawner> {
    SPAWNER.get()
}

async fn follow() {
    match Identity::load_or_create(KEY_NAMESPACE, &format!("Tab5-ADB@{}", host_id())) {
        Ok(identity) => drop(IDENTITY.set(Arc::new(identity))),
        Err(err) => {
            error!(TAG, "identity: {err}");
            return;
        }
    }
    let mut watch = usb_host_rs::watch();
    loop {
        match watch.next().await {
            Event::Arrived(device) if adb::link::usb::has_interface(&device) => {
                info!(TAG, "device {}", device.product());
                *DEVICE.lock() = Some(device);
            }
            Event::Left(device) => {
                let mut current = DEVICE.lock();
                if current.as_ref().is_some_and(|d| d.id() == device.id()) {
                    *current = None;
                    drop(current);
                    let mut connection = CONNECTION.lock();
                    if connection.as_ref().is_some_and(|(route, _)| *route == Route::Usb)
                        && let Some((_, conn)) = connection.take()
                    {
                        conn.close();
                    }
                }
            }
            _ => {}
        }
    }
}

/// The current connection when it goes `route` and is not closed; any other
/// is closed.
fn reuse(route: &Route) -> Option<Connection> {
    let mut current = CONNECTION.lock();
    if let Some((r, conn)) = current.as_ref()
        && r == route
        && !matches!(conn.state(), State::Closed(_))
    {
        return Some(conn.clone());
    }
    if let Some((_, conn)) = current.take() {
        conn.close();
    }
    None
}

fn begin(route: Route, transport: Box<dyn Transport>) -> Result<Connection, String> {
    let (Some(spawner), Some(identity)) = (SPAWNER.get(), IDENTITY.get()) else { return Err("not started".into()) };
    let conn = Connection::connect(transport, identity.clone(), spawner);
    *CONNECTION.lock() = Some((route, conn.clone()));
    let logged = conn.clone();
    drop(spawner.spawn(async move {
        let conn = logged;
        loop {
            let state = conn.state();
            info!(TAG, "{state:?}");
            if matches!(state, State::Closed(_)) {
                return;
            }
            conn.state_changed().await;
        }
    }));
    Ok(conn)
}

/// The plugged-in device's connection, opened on first use and kept until it
/// closes or the device leaves.
pub fn connect() -> Result<Connection, String> {
    let device = DEVICE.lock().clone().ok_or("No ADB device is plugged in.")?;
    if let Some(conn) = reuse(&Route::Usb) {
        return Ok(conn);
    }
    let transport = adb::link::usb::open(&device).map_err(|e| e.to_string())?;
    begin(Route::Usb, transport)
}

/// The connection to adbd at `addr` (`host:port`), kept until it closes.
pub async fn connect_tcp(addr: &str) -> Result<Connection, String> {
    let route = Route::Tcp(addr.into());
    if let Some(conn) = reuse(&route) {
        return Ok(conn);
    }
    join_network().await?;
    let transport = adb::link::tcp::connect(addr).await.map_err(|e| e.to_string())?;
    begin(route, transport)
}

async fn join_network() -> Result<(), String> {
    match devkit_net::start(devkit_net::Config::default()) {
        Ok(()) | Err(devkit_net::Error::AlreadyStarted) => {}
        Err(err) => return Err(format!("network: {err}")),
    }
    let wifi = devkit_net::wifi().ok_or("No Wi-Fi radio.")?;
    if matches!(wifi.state(), WifiState::Connected { .. }) {
        return Ok(());
    }
    wifi.start().await.map_err(|e| format!("Wi-Fi: {e:?}"))?;
    let networks = wifi.scan().await.map_err(|e| format!("Wi-Fi scan: {e:?}"))?;
    let network = networks.first().ok_or("No Wi-Fi network.")?;
    let ssid = String::from_utf8_lossy(&network.ssid).into_owned();
    wifi.connect(&StaConfig::new(&ssid, "")).await.map_err(|e| format!("Wi-Fi {ssid}: {e:?}"))?;
    info!(TAG, "joined {ssid}");
    wifi.sta().wait_ipv4_async().await;
    Ok(())
}

pub fn dbg_exec(cmd: String) {
    let Some(conn) = CONNECTION.lock().as_ref().map(|(_, c)| c.clone()) else { return };
    let Some(spawner) = SPAWNER.get() else { return };
    drop(spawner.spawn(async move {
        match adb::services::exec(&conn, &cmd).await {
            Ok(out) => info!(TAG, "DBG exec: {}", String::from_utf8_lossy(&out).trim_end()),
            Err(err) => error!(TAG, "DBG exec: {err}"),
        }
    }));
}
