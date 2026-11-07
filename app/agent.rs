/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * tab5adb-agent on the connected device (android-agent/docs/protocol.md), and
 * the harness commands that drive it.
 */

use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec::Vec;
use core::ffi::CStr;
use core::fmt::Write;
use core::time::Duration;

use adb::link::{Connection, Stream};
use adb::services::{self, sync::Sync};
use agent_link::{HelloConfig, Link};
use devkit_crypto::hash::{Hash, Sha256};
use idf::executor::{self, JoinHandle, Spawner};
use idf::log::{error, info};
use idf::sync::{Mutex, OnceLock};

use crate::adb_link;

const TAG: &CStr = c"agent";
const JAR: &[u8] = include_bytes!("agent/tab5adb-agent.jar");
const SERVER: &str = "com.tab5adb.agent.Server";
const LISTEN_TIMEOUT: Duration = Duration::from_secs(10);
const REQUEST_TIMEOUT: Duration = Duration::from_secs(1);
const HELLO: HelloConfig = HelloConfig { capabilities: 0, max_message: [256 * 1024, 256 * 1024, 64 * 1024] };

static JAR_PATH: OnceLock<String> = OnceLock::new();
static CURRENT: Mutex<Current> = Mutex::new(Current { generation: 0, state: State::Idle });

#[derive(Clone)]
enum State {
    Idle,
    Starting,
    Ready(Link),
    Failed(String),
}

struct Current {
    generation: u32,
    state: State,
}

/// The agent on the device, from `start` until dropped. One at a time.
pub struct Agent {
    task: Option<JoinHandle<()>>,
}

impl Agent {
    pub fn start(conn: Connection) -> Self {
        let Some(spawner) = adb_link::spawner() else {
            error!(TAG, "adb not started");
            return Self { task: None };
        };
        let generation = {
            let mut current = CURRENT.lock();
            current.generation = current.generation.wrapping_add(1);
            current.state = State::Starting;
            current.generation
        };
        let task = spawner.spawn(async move {
            let state = match bring_up(&conn, spawner).await {
                Ok(link) => {
                    let [major, minor, patch] = link.info().version;
                    info!(TAG, "ready: agent {major}.{minor}.{patch}");
                    State::Ready(link)
                }
                Err(err) => {
                    error!(TAG, "start: {err}");
                    State::Failed(err)
                }
            };
            let mut current = CURRENT.lock();
            if current.generation == generation {
                current.state = state;
            } else if let State::Ready(link) = state {
                link.close();
            }
        });
        Self { task: Some(task) }
    }
}

impl Drop for Agent {
    fn drop(&mut self) {
        if let Some(task) = self.task.take() {
            task.abort();
        }
        let state = {
            let mut current = CURRENT.lock();
            current.generation = current.generation.wrapping_add(1);
            core::mem::replace(&mut current.state, State::Idle)
        };
        if let State::Ready(link) = state {
            link.close();
        }
    }
}

fn jar_path() -> &'static str {
    JAR_PATH.get_or_init(|| {
        let mut path = String::from("/data/local/tmp/tab5adb-agent-");
        for b in &Sha256::digest(JAR).as_ref()[..8] {
            let _ = write!(path, "{b:02x}");
        }
        path.push_str(".jar");
        path
    })
}

async fn bring_up(conn: &Connection, spawner: &Spawner) -> Result<Link, String> {
    let id = adb_link::host_id();
    services::exec(conn, &format!("pkill -f '{SERVER} {id}$'")).await.map_err(|e| format!("pkill: {e}"))?;
    let path = jar_path();
    install(conn, path).await?;
    let mut shell = services::shell(conn, &format!("CLASSPATH={path} app_process / {SERVER} {id}"))
        .await
        .map_err(|e| format!("launch: {e}"))?;
    match executor::timeout(LISTEN_TIMEOUT, wait_listening(&mut shell)).await {
        Ok(Ok(())) => {}
        Ok(Err(output)) => return Err(format!("agent exited: {output}")),
        Err(_) => return Err("agent did not listen".into()),
    }
    drop(spawner.spawn(log_output(shell)));
    Link::open(conn, spawner, id, HELLO).await.map_err(|e| format!("link: {e}"))
}

async fn install(conn: &Connection, path: &str) -> Result<(), String> {
    let mut sync = Sync::open(conn).await.map_err(|e| format!("sync: {e}"))?;
    let stat = sync.stat(path).await.map_err(|e| format!("stat: {e}"))?;
    if !stat.is_file() || stat.size as usize != JAR.len() {
        sync.push_bytes(path, 0o644, 0, JAR).await.map_err(|e| format!("push: {e}"))?;
        info!(TAG, "pushed {path}");
    }
    let _ = sync.close().await;
    Ok(())
}

/// What the agent printed when it exited before `listening`.
async fn wait_listening(shell: &mut Stream) -> Result<(), String> {
    let mut output = Vec::new();
    let mut chunk = [0u8; 256];
    loop {
        let n = shell.read(&mut chunk).await.unwrap_or(0);
        if n == 0 {
            return Err(String::from_utf8_lossy(&output).trim_end().to_string());
        }
        output.extend_from_slice(&chunk[..n]);
        if output.split(|&b| b == b'\n').any(|line| line.trim_ascii() == b"listening") {
            return Ok(());
        }
    }
}

async fn log_output(mut shell: Stream) {
    let mut line = Vec::new();
    let mut chunk = [0u8; 256];
    loop {
        let n = shell.read(&mut chunk).await.unwrap_or(0);
        if n == 0 {
            info!(TAG, "agent exited");
            return;
        }
        for &b in &chunk[..n] {
            if b == b'\n' {
                info!(TAG, "agent: {}", String::from_utf8_lossy(line.trim_ascii()));
                line.clear();
            } else {
                line.push(b);
            }
        }
    }
}

pub fn init() {
    harness_rs::register("agent", command);
}

fn link() -> Result<Link, String> {
    match &CURRENT.lock().state {
        State::Ready(link) => Ok(link.clone()),
        _ => Err("agent not ready".into()),
    }
}

fn command(args: &[&str]) -> Result<Option<String>, String> {
    match args.first().copied() {
        Some("state") => Ok(Some(match &CURRENT.lock().state {
            State::Idle => "idle".into(),
            State::Starting => "starting".into(),
            State::Failed(err) => format!("failed: {err}"),
            State::Ready(link) if link.is_closed() => "closed".into(),
            State::Ready(link) => {
                let info = link.info();
                let [major, minor, patch] = info.version;
                format!("ready {major}.{minor}.{patch} caps={:#x} max={}", info.capabilities, info.max_message)
            }
        })),
        Some("req") => {
            let kind = args.get(1).and_then(|a| u8::from_str_radix(a.trim_start_matches("0x"), 16).ok()).ok_or("usage: agent req <hex type>")?;
            let link = link()?;
            let result = executor::block_on(link.request(kind, &[], REQUEST_TIMEOUT)).map_err(|e| e.to_string())?;
            Ok(Some(format!("{} bytes", result.len())))
        }
        _ => Err("usage: agent state|req <hex type>".into()),
    }
}
