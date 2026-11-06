/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * The USB ADB connection to whichever device is plugged in, and the harness
 * commands that drive it.
 */

use alloc::format;
use alloc::string::{String, ToString};
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::ffi::CStr;
use core::future::{Future, poll_fn};
use core::pin::pin;
use core::task::Poll;
use core::time::Duration;

use adb::link::{Connection, Identity, State};
use adb::services::sync::Sync;
use idf::executor::{self, Spawner};
use idf::log::{error, info};
use idf::sync::{Mutex, OnceLock};
use usb_host_rs::{Device, Event};
use idf::thread::Builder;
use idf::time::Instant;

const TAG: &CStr = c"adb_link";
const KEY_NAMESPACE: &CStr = c"adb";
const PUSH_PATH: &str = "/data/local/tmp/tab5adb.bin";
static SPAWNER: OnceLock<Spawner> = OnceLock::new();
static IDENTITY: OnceLock<Arc<Identity>> = OnceLock::new();
static DEVICE: Mutex<Option<Device>> = Mutex::new(None);
static CONNECTION: Mutex<Option<Connection>> = Mutex::new(None);

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
    harness_rs::register("adb", command);
}

async fn follow() {
    match Identity::load_or_create(KEY_NAMESPACE) {
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
                    if let Some(conn) = CONNECTION.lock().take() {
                        conn.close();
                    }
                }
            }
            _ => {}
        }
    }
}

/// The device's connection, opened on first use and kept until it closes or
/// the device leaves.
pub fn connect() -> Result<Connection, String> {
    let device = DEVICE.lock().clone().ok_or("No ADB device is plugged in.")?;
    let (Some(spawner), Some(identity)) = (SPAWNER.get(), IDENTITY.get()) else { return Err("not started".into()) };
    {
        let mut current = CONNECTION.lock();
        if let Some(conn) = current.as_ref()
            && !matches!(conn.state(), State::Closed(_))
        {
            return Ok(conn.clone());
        }
        *current = None;
    }
    let transport = adb::link::usb::open(&device).map_err(|e| e.to_string())?;
    let conn = Connection::connect(transport, identity.clone(), spawner);
    *CONNECTION.lock() = Some(conn.clone());
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

fn connection() -> Result<Connection, String> {
    CONNECTION.lock().clone().ok_or_else(|| "no device".to_string())
}

fn number(args: &[&str], at: usize, default: usize) -> Result<usize, String> {
    args.get(at).map_or(Ok(default), |a| a.parse().map_err(|_| format!("bad number {a}")))
}

fn pattern(len: usize) -> Vec<u8> {
    (0..len).map(|i| (i * 7 + i / 251) as u8).collect()
}

fn rate(bytes: usize, start: Instant) -> String {
    let us = start.elapsed().as_micros().max(1) as u64;
    format!("{bytes} bytes in {} ms, {} kB/s", us / 1000, bytes as u64 * 1000 / us)
}

fn command(args: &[&str]) -> Result<Option<String>, String> {
    let rest = args.get(1..).unwrap_or(&[]);
    match args.first().copied() {
        Some("state") => {
            let conn = connection()?;
            Ok(Some(format!("{:?} \"{}\"", conn.state(), conn.banner())))
        }
        Some("wait") => {
            let conn = connection()?;
            let ms = number(rest, 0, 30000)? as u64;
            match executor::block_on(executor::timeout(Duration::from_millis(ms), conn.online())) {
                Ok(Ok(())) => Ok(Some("online".into())),
                Ok(Err(err)) => Err(err.to_string()),
                Err(_) => Err(format!("still {:?}", conn.state())),
            }
        }
        Some("close") => {
            connection()?.close();
            Ok(None)
        }
        Some("connect") => connect().map(|_| None),
        Some("exec") => {
            let conn = connection()?;
            let out = executor::block_on(adb::services::exec(&conn, &rest.join(" "))).map_err(|e| e.to_string())?;
            let text = String::from_utf8_lossy(&out);
            Ok(Some(format!("{} bytes: {}", out.len(), text.trim_end().replace('\n', "\\n"))))
        }
        Some("echo") => executor::block_on(echo(number(rest, 0, 4096)?)).map(Some),
        Some("push") => executor::block_on(push(number(rest, 0, 1 << 20)?)).map(Some),
        Some("pull") => executor::block_on(pull()).map(Some),
        Some("ls") => executor::block_on(list(rest.first().copied().unwrap_or("/data/local/tmp"))).map(Some),
        _ => Err("usage: adb state|wait [ms]|close|connect|exec <cmd>|echo [bytes]|push [bytes]|pull|ls [path]".into()),
    }
}

async fn join<A: Future, B: Future>(a: A, b: B) -> (A::Output, B::Output) {
    let (mut a, mut b) = (pin!(a), pin!(b));
    let (mut ra, mut rb) = (None, None);
    poll_fn(|cx| {
        if ra.is_none()
            && let Poll::Ready(v) = a.as_mut().poll(cx)
        {
            ra = Some(v);
        }
        if rb.is_none()
            && let Poll::Ready(v) = b.as_mut().poll(cx)
        {
            rb = Some(v);
        }
        if ra.is_some() && rb.is_some() { Poll::Ready(()) } else { Poll::Pending }
    })
    .await;
    (ra.unwrap(), rb.unwrap())
}

/// Through `cat`, reading while writing so neither side's buffer fills.
async fn echo(len: usize) -> Result<String, String> {
    let conn = connection()?;
    let stream = conn.open("exec:cat").await.map_err(|e| e.to_string())?;
    let (mut reader, mut writer) = stream.into_split();
    let data = pattern(len);
    let start = Instant::now();
    let sent = writer.write(&data);
    let received = async {
        let mut got = alloc::vec![0u8; len];
        reader.read_exact(&mut got).await.map(|()| got)
    };
    let (sent, received) = join(sent, received).await;
    sent.map_err(|e| format!("write: {e}"))?;
    let got = received.map_err(|e| format!("read: {e}"))?;
    let elapsed = rate(len * 2, start);
    let _ = writer.close().await;
    if got != data {
        return Err("echo differs".into());
    }
    Ok(elapsed)
}

async fn push(len: usize) -> Result<String, String> {
    let conn = connection()?;
    let data = pattern(len);
    let mut sync = Sync::open(&conn).await.map_err(|e| e.to_string())?;
    let start = Instant::now();
    sync.push_bytes(PUSH_PATH, 0o644, 0, &data).await.map_err(|e| format!("push: {e}"))?;
    let elapsed = rate(len, start);
    let stat = sync.stat(PUSH_PATH).await.map_err(|e| format!("stat: {e}"))?;
    let _ = sync.close().await;
    if stat.size as usize != len || !stat.is_file() {
        return Err(format!("stat says {stat:?}"));
    }
    Ok(elapsed)
}

async fn pull() -> Result<String, String> {
    let conn = connection()?;
    let mut sync = Sync::open(&conn).await.map_err(|e| e.to_string())?;
    let mut got = Vec::new();
    let start = Instant::now();
    sync.pull(PUSH_PATH, |chunk| {
        got.extend_from_slice(chunk);
        Ok(())
    })
    .await
    .map_err(|e| format!("pull: {e}"))?;
    let elapsed = rate(got.len(), start);
    let _ = sync.close().await;
    if got != pattern(got.len()) {
        return Err("pulled data differs from what push sends".into());
    }
    Ok(elapsed)
}

async fn list(path: &str) -> Result<String, String> {
    let conn = connection()?;
    let mut sync = Sync::open(&conn).await.map_err(|e| e.to_string())?;
    let entries = sync.list(path).await.map_err(|e| e.to_string())?;
    let _ = sync.close().await;
    let names: Vec<&str> = entries.iter().map(|e| e.name.as_str()).filter(|n| *n != "." && *n != "..").collect();
    Ok(format!("{} entries: {}", names.len(), names.join(" ")))
}
