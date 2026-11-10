/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * tab5adb-agent on the connected device (android-agent/docs/protocol.md).
 */

use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec::Vec;
use core::ffi::CStr;
use core::fmt::{Display, Write};
use core::future::Future;
use core::pin::pin;
use core::sync::atomic::{AtomicU32, Ordering};
use core::time::Duration;

use adb::link::{Connection, Stream};
use adb::services::{self, sync::Sync};
use agent_link::protocol::{CAP_APPS, CAP_INPUT, CAP_MEDIA, CAP_VIDEO};
use agent_link::{HelloConfig, Link};
use devkit_crypto::hash::{Hash, Sha256};
use idf::executor::{self, Spawner};
use idf::log::info;
use idf::sync::{Mutex, OnceLock};
use idf::time::Instant;

use crate::adb_link;

const TAG: &CStr = c"agent";
const JAR: &[u8] = include_bytes!("agent/tab5adb-agent.jar");
const SERVER: &str = "com.tab5adb.agent.Server";
const STEP_TIMEOUT: Duration = Duration::from_secs(5);
const PUSH_STALL: Duration = Duration::from_secs(5);
const LISTEN_TIMEOUT: Duration = Duration::from_secs(10);
/// Opening the control stream, then HELLO's own 3 s.
const LINK_TIMEOUT: Duration = Duration::from_secs(8);
const HELLO: HelloConfig = HelloConfig { capabilities: CAP_VIDEO | CAP_INPUT | CAP_MEDIA | CAP_APPS, max_message: [2 * 1024 * 1024, 512 * 1024, 64 * 1024] };

static JAR_PATH: OnceLock<String> = OnceLock::new();
static CURRENT: Mutex<Current> = Mutex::new(Current { generation: 0, link: None });

struct Current {
    generation: u32,
    link: Option<Link>,
}

/// The agent on the device, from `start` until dropped. One at a time.
pub struct Agent {
    generation: u32,
    link: Link,
}

impl Agent {
    /// Dropping the future stops the bring-up.
    pub async fn start(conn: Connection) -> Result<Self, String> {
        let spawner = adb_link::spawner().ok_or("adb not started")?;
        let mut task = devkit::guard(spawner.spawn(async move { bring_up(&conn, spawner).await }), |task| task.abort());
        let link = (&mut *task).await?;
        let [major, minor, patch] = link.info().version;
        info!(TAG, "ready: agent {major}.{minor}.{patch}");
        let mut current = CURRENT.lock();
        current.generation = current.generation.wrapping_add(1);
        if let Some(old) = current.link.replace(link.clone()) {
            old.close();
        }
        Ok(Self { generation: current.generation, link })
    }

    pub fn link(&self) -> &Link {
        &self.link
    }
}

impl Drop for Agent {
    fn drop(&mut self) {
        let mut current = CURRENT.lock();
        if current.generation == self.generation {
            current.link = None;
        }
        drop(current);
        self.link.close();
    }
}

/// The agent's link once it is up and still open.
pub fn link() -> Option<Link> {
    CURRENT.lock().link.clone().filter(|link| !link.is_closed())
}

/// The same, when both ends gave `capabilities` in HELLO.
pub fn link_with(capabilities: u32) -> Option<Link> {
    link().filter(|link| link.supports(capabilities))
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
    step("pkill", STEP_TIMEOUT, services::exec(conn, &format!("pkill -f '{SERVER} {id}$'"))).await?;
    let path = jar_path();
    install(conn, path).await?;
    let mut shell = step("launch", STEP_TIMEOUT, services::shell(conn, &format!("CLASSPATH={path} app_process / {SERVER} {id}"))).await?;
    match executor::timeout(LISTEN_TIMEOUT, wait_listening(&mut shell)).await {
        Ok(Ok(())) => {}
        Ok(Err(output)) => return Err(format!("agent exited: {output}")),
        Err(_) => return Err("agent did not listen".into()),
    }
    drop(spawner.spawn(log_output(shell)));
    step("link", LINK_TIMEOUT, Link::open(conn, spawner, id, HELLO)).await
}

async fn step<T, E: Display>(name: &str, timeout: Duration, future: impl Future<Output = Result<T, E>>) -> Result<T, String> {
    match executor::timeout(timeout, future).await {
        Ok(result) => result.map_err(|e| format!("{name}: {e}")),
        Err(_) => Err(format!("{name}: timed out")),
    }
}

async fn install(conn: &Connection, path: &str) -> Result<(), String> {
    let mut sync = step("sync", STEP_TIMEOUT, Sync::open(conn)).await?;
    let stat = step("stat", STEP_TIMEOUT, sync.stat(path)).await?;
    if !stat.is_file() || stat.size as usize != JAR.len() {
        push(&mut sync, path).await?;
        info!(TAG, "pushed {path}");
    }
    let _ = executor::timeout(STEP_TIMEOUT, sync.close()).await;
    Ok(())
}

/// Fails once `PUSH_STALL` passes without a chunk going out.
async fn push(sync: &mut Sync, path: &str) -> Result<(), String> {
    let start = Instant::now();
    let progress = AtomicU32::new(0);
    let since = || start.elapsed().saturating_sub(Duration::from_millis(u64::from(progress.load(Ordering::Relaxed))));
    let mut rest = JAR;
    let mut push = pin!(sync.push(path, 0o644, 0, |buf| {
        progress.store(start.elapsed().as_millis() as u32, Ordering::Relaxed);
        let n = buf.len().min(rest.len());
        buf[..n].copy_from_slice(&rest[..n]);
        rest = &rest[n..];
        Ok(n)
    }));
    loop {
        match executor::timeout(PUSH_STALL.saturating_sub(since()), push.as_mut()).await {
            Ok(result) => return result.map_err(|e| format!("push: {e}")),
            Err(_) if since() >= PUSH_STALL => return Err("push: stalled".into()),
            Err(_) => {}
        }
    }
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
            for line in output.split(|&b| b == b'\n').map(<[u8]>::trim_ascii).filter(|line| !line.is_empty() && *line != b"listening") {
                info!(TAG, "agent: {}", String::from_utf8_lossy(line));
            }
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
                if !line.trim_ascii().is_empty() {
                    info!(TAG, "agent: {}", String::from_utf8_lossy(line.trim_ascii()));
                }
                line.clear();
            } else {
                line.push(b);
            }
        }
    }
}
