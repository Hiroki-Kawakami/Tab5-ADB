/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Deleting and copying files on their own task, one operation at a time.
 */

use alloc::boxed::Box;
use alloc::string::{String, ToString};
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::ffi::CStr;
use core::future::{Future, poll_fn};
use core::pin::{Pin, pin};
use core::sync::atomic::{AtomicBool, Ordering};
use core::task::Poll;
use core::time::Duration;

use adb::link::Connection;
use adb::services::sync::Sync;
use bsp_rs::rtc::Rtc;
use idf::executor::Spawner;
use idf::fs::{File, OpenOptions};
use idf::heap::Caps;
use idf::log::error;
use idf::sync::{Mutex, OnceLock};
use idf::thread::Builder;

use crate::files::{self, c_path, name_of, run, shell_quote};

const TAG: &CStr = c"file_ops";
const STACK: usize = 16384;
/// A whole number of 16 KiB blocks, so the card is read and written in them.
const CHUNK: usize = 1024 * 1024;
const CANCEL_POLL: Duration = Duration::from_millis(100);
const MKDIR_BATCH: usize = 2048;
const INSTALL_TEMP: &str = "/data/local/tmp/tab5adb-install.apk";

static SPAWNER: OnceLock<Spawner> = OnceLock::new();
static RTC: OnceLock<&'static Rtc> = OnceLock::new();
static RUNNING: AtomicBool = AtomicBool::new(false);

pub fn set_rtc(rtc: &'static Rtc) {
    let _ = RTC.set(rtc);
}

#[derive(Clone, Default)]
pub struct Progress {
    /// Files done, of `total`.
    pub done: usize,
    pub total: usize,
    pub bytes_done: u64,
    /// 0 when only `done` tells how far it is.
    pub bytes_total: u64,
    /// How far it is cannot be told.
    pub indeterminate: bool,
    pub current: String,
    pub finished: bool,
    pub cancelled: bool,
    pub error: Option<String>,
}

impl Progress {
    pub fn fraction(&self) -> Option<f32> {
        if self.indeterminate {
            None
        } else if self.bytes_total > 0 {
            Some(self.bytes_done as f32 / self.bytes_total as f32)
        } else if self.total > 0 {
            Some(self.done as f32 / self.total as f32)
        } else {
            Some(0.0)
        }
    }
}

struct Shared {
    progress: Mutex<Progress>,
    cancel: AtomicBool,
}

#[derive(Clone)]
pub struct Operation(Arc<Shared>);

impl Operation {
    pub fn progress(&self) -> Progress {
        self.0.progress.lock().clone()
    }

    /// Stops before the next file, or within the one being copied, whose
    /// partial copy is removed; what is done stays done.
    pub fn cancel(&self) {
        self.0.cancel.store(true, Ordering::Release);
    }

    fn cancelled(&self) -> bool {
        self.0.cancel.load(Ordering::Acquire)
    }

    fn update(&self, f: impl FnOnce(&mut Progress)) {
        f(&mut self.0.progress.lock());
    }
}

#[derive(Clone)]
pub enum Endpoint {
    Android(Connection),
    Sd,
}

pub struct Copy {
    pub source: String,
    /// Must not exist yet.
    pub target: String,
}

/// `None` while another operation runs or when its task cannot start.
pub fn delete(at: Endpoint, paths: Vec<String>) -> Option<Operation> {
    start(move |op| async move {
        op.update(|p| p.total = paths.len());
        for (i, path) in paths.iter().enumerate() {
            if op.cancelled() {
                return Err(None);
            }
            op.update(|p| {
                p.done = i;
                p.current = name_of(path).to_string();
            });
            match &at {
                Endpoint::Android(conn) => run(conn, &alloc::format!("rm -rf -- {}", shell_quote(path))).await,
                Endpoint::Sd => remove_sd(&op, path),
            }
            .map_err(|err| Some(alloc::format!("{}\n{err}", name_of(path))))?;
        }
        op.update(|p| p.done = paths.len());
        Ok(())
    })
}

/// Directories go with their contents.
pub fn copy(from: Endpoint, to: Endpoint, copies: Vec<Copy>) -> Option<Operation> {
    start(move |op| async move {
        match (&from, &to) {
            (Endpoint::Android(conn), Endpoint::Android(_)) => copy_on_device(&op, conn, &copies).await,
            _ => copy_between(&op, &from, &to, &copies).await,
        }
    })
}

/// Installs the APK at `path` on the device of `conn`, from a copy in the
/// device's temporary folder.
pub fn install(from: Endpoint, conn: Connection, path: String) -> Option<Operation> {
    start(move |op| async move {
        let name = name_of(&path).to_string();
        op.update(|p| {
            p.total = 1;
            p.current = name.clone();
        });
        match from {
            // The package manager cannot open files on shared storage, so it
            // installs from a copy where it can.
            Endpoint::Android(_) => {
                op.update(|p| p.indeterminate = true);
                run(&conn, &alloc::format!("cp -- {} {INSTALL_TEMP}", shell_quote(&path))).await.map_err(|err| failed(&path, err))?;
            }
            Endpoint::Sd => {
                let (size, _) = files::Volume::Sd.stat(&path).await.map_err(|err| failed(&path, err))?;
                op.update(|p| p.bytes_total = size);
                let mut buf = idf::heap::zeroed_slice::<u8>(CHUNK, Caps::SPIRAM | Caps::CACHE_ALIGNED).map_err(|err| failed(&path, alloc::format!("buffer: {err}")))?;
                let mut session = Sync::open(&conn).await.map_err(|err| failed(&path, err))?;
                match push(&op, &mut buf, &mut session, &conn, &path, INSTALL_TEMP).await {
                    Ok(()) => {}
                    Err(_) if op.cancelled() => return Err(None),
                    Err(err) => return Err(failed(&path, err)),
                }
                let _ = session.close().await;
            }
        }
        if op.cancelled() {
            return Err(None);
        }
        op.update(|p| p.indeterminate = true);
        let out = adb::services::exec(&conn, &alloc::format!("pm install -r {INSTALL_TEMP} 2>&1")).await;
        let _ = run(&conn, &alloc::format!("rm -f {INSTALL_TEMP}")).await;
        let out = String::from_utf8_lossy(&out.map_err(|err| failed(&path, err))?).trim().to_string();
        if !out.lines().any(|line| line.trim() == "Success") {
            error!(TAG, "pm install {path}: {out}");
            let reason = out.lines().find(|line| line.starts_with("Failure")).or_else(|| out.lines().next()).unwrap_or("The installation failed.");
            return Err(failed(&path, reason));
        }
        op.update(|p| p.done = 1);
        Ok(())
    })
}

fn start<F, Fut>(job: F) -> Option<Operation>
where
    F: FnOnce(Operation) -> Fut + Send + 'static,
    Fut: Future<Output = Result<(), Option<String>>> + Send + 'static,
{
    if RUNNING.swap(true, Ordering::AcqRel) {
        return None;
    }
    let Some(spawner) = spawner() else {
        RUNNING.store(false, Ordering::Release);
        return None;
    };
    let op = Operation(Arc::new(Shared { progress: Mutex::new(Progress::default()), cancel: AtomicBool::new(false) }));
    let task = op.clone();
    drop(spawner.spawn(async move {
        let result = job(task.clone()).await;
        task.update(|p| {
            match result {
                Ok(()) => {}
                Err(None) => p.cancelled = true,
                Err(Some(err)) => p.error = Some(err),
            }
            p.finished = true;
        });
        RUNNING.store(false, Ordering::Release);
    }));
    Some(op)
}

fn spawner() -> Option<&'static Spawner> {
    if let Some(spawner) = SPAWNER.get() {
        return Some(spawner);
    }
    match Builder::new().name("files").stack_size(STACK).spawn_local_executor(|_| {}) {
        Ok(spawner) => Some(SPAWNER.get_or_init(|| spawner)),
        Err(err) => {
            error!(TAG, "executor: {err}");
            None
        }
    }
}

fn failed(path: &str, message: impl core::fmt::Display) -> Option<String> {
    Some(alloc::format!("{}\n{message}", name_of(path)))
}

fn remove_sd(op: &Operation, path: &str) -> Result<(), String> {
    if op.cancelled() {
        return Err(String::from("Cancelled"));
    }
    let c = c_path(path)?;
    let meta = idf::fs::metadata(&c).map_err(|err| alloc::format!("stat: {err}"))?;
    if meta.is_dir() {
        let names: Vec<String> = idf::fs::read_dir(&c).map_err(|err| alloc::format!("read_dir: {err}"))?.map(|e| e.file_name().to_string_lossy().into_owned()).collect();
        for name in names {
            remove_sd(op, &files::join(path, &name))?;
        }
        idf::fs::remove_dir(&c).map_err(|err| {
            error!(TAG, "rmdir {path}: {err}");
            String::from("The folder could not be deleted.")
        })
    } else {
        idf::fs::remove_file(&c).map_err(|err| {
            error!(TAG, "unlink {path}: {err}");
            String::from("The file could not be deleted.")
        })
    }
}

/// One `cp -r` per item, which the device runs without telling how far it is.
async fn copy_on_device(op: &Operation, conn: &Connection, copies: &[Copy]) -> Result<(), Option<String>> {
    op.update(|p| {
        p.total = copies.len();
        p.indeterminate = true;
    });
    for (i, copy) in copies.iter().enumerate() {
        if op.cancelled() {
            return Err(None);
        }
        op.update(|p| {
            p.done = i;
            p.current = name_of(&copy.source).to_string();
        });
        let command = alloc::format!("cp -r -- {} {} 2>&1 & echo $!; wait $!; echo \":$?\"", shell_quote(&copy.source), shell_quote(&copy.target));
        let mut stream = adb::services::shell(conn, &command).await.map_err(|err| failed(&copy.source, err))?;
        let mut out = Vec::new();
        let mut buf = alloc::vec![0u8; 1024];
        loop {
            let n = match until_cancelled(op, pin!(stream.read(&mut buf))).await {
                Some(result) => result.map_err(|err| failed(&copy.source, err))?,
                None => {
                    let pid = String::from_utf8_lossy(&out).lines().next().unwrap_or("").trim().to_string();
                    if !pid.is_empty() && pid.bytes().all(|b| b.is_ascii_digit()) {
                        let _ = run(conn, &alloc::format!("kill {pid}")).await;
                    }
                    let _ = run(conn, &alloc::format!("rm -rf -- {}", shell_quote(&copy.target))).await;
                    return Err(None);
                }
            };
            if n == 0 {
                break;
            }
            out.extend_from_slice(&buf[..n]);
        }
        let out = String::from_utf8_lossy(&out);
        let mut lines = out.trim_end().lines();
        lines.next();
        let rest: Vec<&str> = lines.collect();
        let status = rest.last().and_then(|l| l.strip_prefix(':')).unwrap_or("1");
        if status != "0" {
            let message = rest[..rest.len().saturating_sub(1)].join("\n");
            error!(TAG, "cp {} -> {}: {message} ({status})", copy.source, copy.target);
            return Err(failed(&copy.source, if message.is_empty() { alloc::format!("exit status {status}") } else { message }));
        }
    }
    op.update(|p| p.done = copies.len());
    Ok(())
}

/// `future`'s output, or `None` once `op` is cancelled first.
async fn until_cancelled<F: Future>(op: &Operation, mut future: Pin<&mut F>) -> Option<F::Output> {
    loop {
        let mut tick = pin!(idf::executor::sleep(CANCEL_POLL));
        let ready = poll_fn(|cx| {
            if let Poll::Ready(output) = future.as_mut().poll(cx) {
                return Poll::Ready(Some(output));
            }
            if tick.as_mut().poll(cx).is_ready() {
                return Poll::Ready(None);
            }
            Poll::Pending
        })
        .await;
        match ready {
            Some(output) => return Some(output),
            None if op.cancelled() => return None,
            None => {}
        }
    }
}

enum Step {
    Dir(String),
    File { source: String, target: String, size: u64 },
}

async fn copy_between(op: &Operation, from: &Endpoint, to: &Endpoint, copies: &[Copy]) -> Result<(), Option<String>> {
    let mut session = match from.connection().or_else(|| to.connection()) {
        Some(conn) => Some(Sync::open(conn).await.map_err(|err| failed(&copies[0].source, err))?),
        None => None,
    };
    let mut buf = idf::heap::zeroed_slice::<u8>(CHUNK, Caps::SPIRAM | Caps::CACHE_ALIGNED).map_err(|err| failed(&copies[0].source, alloc::format!("buffer: {err}")))?;
    let mut steps = Vec::new();
    for copy in copies {
        if op.cancelled() {
            return Err(None);
        }
        op.update(|p| p.current = name_of(&copy.source).to_string());
        scan(from, session.as_mut(), &copy.source, &copy.target, &mut steps).await.map_err(|err| failed(&copy.source, err))?;
    }
    let files: Vec<_> = steps.iter().filter(|s| matches!(s, Step::File { .. })).collect();
    let bytes_total = files.iter().map(|s| if let Step::File { size, .. } = s { *size } else { 0 }).sum();
    op.update(|p| {
        p.total = files.len();
        p.bytes_total = bytes_total;
    });
    if let Endpoint::Android(conn) = to {
        let dirs: Vec<String> = steps.iter().filter_map(|s| if let Step::Dir(dir) = s { Some(shell_quote(dir)) } else { None }).collect();
        let mut batch = String::new();
        for dir in dirs {
            if !batch.is_empty() && batch.len() + dir.len() > MKDIR_BATCH {
                run(conn, &alloc::format!("mkdir -p --{batch}")).await.map_err(|err| failed(&copies[0].target, err))?;
                batch.clear();
            }
            batch.push(' ');
            batch.push_str(&dir);
        }
        if !batch.is_empty() {
            run(conn, &alloc::format!("mkdir -p --{batch}")).await.map_err(|err| failed(&copies[0].target, err))?;
        }
    }
    let mut done = 0;
    for step in &steps {
        if op.cancelled() {
            return Err(None);
        }
        match step {
            Step::Dir(dir) => {
                if let Endpoint::Sd = to {
                    let c = c_path(dir).map_err(|err| failed(dir, err))?;
                    if idf::fs::metadata(&c).is_err() {
                        idf::fs::create_dir(&c).map_err(|err| failed(dir, alloc::format!("mkdir: {err}")))?;
                    }
                }
            }
            Step::File { source, target, .. } => {
                op.update(|p| {
                    p.done = done;
                    p.current = name_of(source).to_string();
                });
                let result = match (from, to) {
                    (Endpoint::Sd, Endpoint::Sd) => copy_sd(op, &mut buf, source, target),
                    (Endpoint::Android(_), Endpoint::Sd) => pull(op, &mut buf, session.as_mut().unwrap(), source, target).await,
                    (Endpoint::Sd, Endpoint::Android(conn)) => push(op, &mut buf, session.as_mut().unwrap(), conn, source, target).await,
                    (Endpoint::Android(_), Endpoint::Android(_)) => unreachable!(),
                };
                match result {
                    Ok(()) => done += 1,
                    Err(_) if op.cancelled() => return Err(None),
                    Err(err) => return Err(failed(source, err)),
                }
            }
        }
    }
    op.update(|p| p.done = done);
    if let Some(session) = session {
        let _ = session.close().await;
    }
    Ok(())
}

impl Endpoint {
    fn connection(&self) -> Option<&Connection> {
        match self {
            Endpoint::Android(conn) => Some(conn),
            Endpoint::Sd => None,
        }
    }
}

async fn scan(from: &Endpoint, session: Option<&mut Sync>, source: &str, target: &str, steps: &mut Vec<Step>) -> Result<(), String> {
    match from {
        Endpoint::Sd => scan_sd(source, target, steps),
        Endpoint::Android(conn) => {
            let session = session.ok_or("no session")?;
            let stat = if conn.has_feature(adb::services::sync::FEATURE_STAT_V2) { session.stat_v2(source).await } else { session.stat(source).await }.map_err(|err| err.to_string())?;
            if !stat.exists() {
                return Err(String::from("The item no longer exists."));
            }
            if !stat.is_dir() {
                steps.push(Step::File { source: source.to_string(), target: target.to_string(), size: stat.size });
                return Ok(());
            }
            scan_android(session, source, target, steps).await
        }
    }
}

fn scan_android<'a>(session: &'a mut Sync, source: &'a str, target: &'a str, steps: &'a mut Vec<Step>) -> Pin<Box<dyn Future<Output = Result<(), String>> + Send + 'a>> {
    Box::pin(async move {
        steps.push(Step::Dir(target.to_string()));
        let entries = session.list(source).await.map_err(|err| err.to_string())?;
        for entry in entries.into_iter().filter(|e| e.name != "." && e.name != "..") {
            let (source, target) = (files::join(source, &entry.name), files::join(target, &entry.name));
            if entry.stat.is_dir() {
                scan_android(session, &source, &target, steps).await?;
            } else if entry.stat.is_file() {
                steps.push(Step::File { source, target, size: entry.stat.size });
            }
            // A link inside a tree is left out: following one can loop.
        }
        Ok(())
    })
}

fn scan_sd(source: &str, target: &str, steps: &mut Vec<Step>) -> Result<(), String> {
    let c = c_path(source)?;
    let meta = idf::fs::metadata(&c).map_err(|err| alloc::format!("stat: {err}"))?;
    if !meta.is_dir() {
        steps.push(Step::File { source: source.to_string(), target: target.to_string(), size: meta.len() });
        return Ok(());
    }
    steps.push(Step::Dir(target.to_string()));
    let names: Vec<String> = idf::fs::read_dir(&c).map_err(|err| alloc::format!("read_dir: {err}"))?.map(|e| e.file_name().to_string_lossy().into_owned()).collect();
    for name in names {
        scan_sd(&files::join(source, &name), &files::join(target, &name), steps)?;
    }
    Ok(())
}

fn create_sd(path: &str) -> Result<File, String> {
    OpenOptions::new().write(true).create(true).truncate(true).open(&c_path(path)?).map_err(|err| alloc::format!("create: {err}"))
}

fn remove_partial_sd(path: &str) {
    if let Ok(c) = c_path(path) {
        let _ = idf::fs::remove_file(&c);
    }
}

fn copy_sd(op: &Operation, buf: &mut [u8], source: &str, target: &str) -> Result<(), String> {
    let input = File::open(&c_path(source)?).map_err(|err| alloc::format!("open: {err}"))?;
    let output = create_sd(target)?;
    let result = loop {
        if op.cancelled() {
            break Err(String::from("Cancelled"));
        }
        let n = match input.read(buf) {
            Ok(0) => break Ok(()),
            Ok(n) => n,
            Err(err) => break Err(alloc::format!("read: {err}")),
        };
        if let Err(err) = output.write_all(&buf[..n]) {
            break Err(alloc::format!("write: {err}"));
        }
        op.update(|p| p.bytes_done += n as u64);
    };
    drop(output);
    if result.is_err() {
        remove_partial_sd(target);
    }
    result
}

/// What arrives is gathered in `buf` and written a whole buffer at a time.
async fn pull(op: &Operation, buf: &mut [u8], session: &mut Sync, source: &str, target: &str) -> Result<(), String> {
    let output = create_sd(target)?;
    let mut filled = 0;
    let write = |data: &[u8]| {
        output.write_all(data).map_err(|err| {
            error!(TAG, "write {target}: {err}");
            adb::Error::Canceled
        })
    };
    let result = session
        .pull(source, |mut data| {
            if op.cancelled() {
                return Err(adb::Error::Canceled);
            }
            op.update(|p| p.bytes_done += data.len() as u64);
            while !data.is_empty() {
                let n = (buf.len() - filled).min(data.len());
                buf[filled..filled + n].copy_from_slice(&data[..n]);
                filled += n;
                data = &data[n..];
                if filled == buf.len() {
                    write(buf)?;
                    filled = 0;
                }
            }
            Ok(())
        })
        .await
        .and_then(|()| write(&buf[..filled]));
    drop(output);
    if result.is_err() {
        remove_partial_sd(target);
    }
    result.map_err(|err| err.to_string())
}

/// The file is read a whole `buf` at a time and handed out as the session asks.
async fn push(op: &Operation, buf: &mut [u8], session: &mut Sync, conn: &Connection, source: &str, target: &str) -> Result<(), String> {
    let input = File::open(&c_path(source)?).map_err(|err| alloc::format!("open: {err}"))?;
    let mtime = RTC.get().and_then(|rtc| rtc.now().ok()).map_or(0, |now| now.seconds().max(0) as u32);
    let (mut start, mut end) = (0, 0);
    let result = session
        .push(target, 0o644, mtime, |out| {
            if op.cancelled() {
                return Err(adb::Error::Canceled);
            }
            if start == end {
                end = input.read(buf).map_err(|err| {
                    error!(TAG, "read {source}: {err}");
                    adb::Error::Canceled
                })?;
                start = 0;
            }
            let n = out.len().min(end - start);
            out[..n].copy_from_slice(&buf[start..start + n]);
            start += n;
            op.update(|p| p.bytes_done += n as u64);
            Ok(n)
        })
        .await;
    if result.is_err() {
        let _ = run(conn, &alloc::format!("rm -f -- {}", shell_quote(target))).await;
    }
    result.map_err(|err| err.to_string())
}

impl From<&files::Volume> for Endpoint {
    fn from(volume: &files::Volume) -> Self {
        match volume.connection() {
            Some(conn) => Endpoint::Android(conn),
            None => Endpoint::Sd,
        }
    }
}
