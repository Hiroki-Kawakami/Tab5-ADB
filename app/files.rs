/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::ffi::CString;
use alloc::string::{String, ToString};
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::cmp::Ordering;
use core::ffi::CStr;

use adb::link::Connection;
use adb::services::sync::{self, Sync};
use bsp_rs::sd::{MountConfig, Sd, Space};
use idf::log::error;
use idf::sync::{Mutex, OnceLock};

const TAG: &CStr = c"files";
pub const SD_ROOT: &str = "/sd";
const SD_ROOT_C: &CStr = c"/sd";

static SD: OnceLock<&'static Sd> = OnceLock::new();

pub fn set_sd(sd: &'static Sd) {
    let _ = SD.set(sd);
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    Dir,
    File,
    /// Not known yet whether it leads to a directory.
    Link,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum FileType {
    Apk,
    Image,
    Video,
    Audio,
    Text,
    Other,
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Entry {
    pub name: String,
    pub kind: Kind,
}

impl Entry {
    pub fn file_type(&self) -> FileType {
        file_type(&self.name)
    }
}

pub fn file_type(name: &str) -> FileType {
    let Some((_, ext)) = name.rsplit_once('.') else { return FileType::Other };
    let ext = ext.to_ascii_lowercase();
    match ext.as_str() {
        "apk" | "apks" | "xapk" => FileType::Apk,
        "jpg" | "jpeg" | "png" | "gif" | "bmp" | "webp" | "heic" => FileType::Image,
        "mp4" | "mkv" | "webm" | "avi" | "mov" | "3gp" => FileType::Video,
        "mp3" | "m4a" | "aac" | "flac" | "ogg" | "opus" | "wav" => FileType::Audio,
        "txt" | "md" | "log" | "json" | "xml" | "csv" | "ini" | "conf" | "sh" => FileType::Text,
        _ => FileType::Other,
    }
}

/// Directories and links first, then by name without regard to case.
pub fn sort(entries: &mut [Entry]) {
    entries.sort_by(|a, b| {
        let group = |e: &Entry| if e.kind == Kind::File { 1 } else { 0 };
        group(a).cmp(&group(b)).then_with(|| compare_names(&a.name, &b.name))
    });
}

fn compare_names(a: &str, b: &str) -> Ordering {
    let fold = |s: &str| s.chars().flat_map(char::to_lowercase).collect::<String>();
    fold(a).cmp(&fold(b)).then_with(|| a.cmp(b))
}

pub fn join(dir: &str, name: &str) -> String {
    if dir.ends_with('/') { alloc::format!("{dir}{name}") } else { alloc::format!("{dir}/{name}") }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Storage {
    Android,
    Sd,
}

#[derive(Clone)]
pub enum Volume {
    Android(Android),
    Sd,
}

impl Volume {
    pub fn storage(&self) -> Storage {
        match self {
            Volume::Android(_) => Storage::Android,
            Volume::Sd => Storage::Sd,
        }
    }

    pub fn connection(&self) -> Option<Connection> {
        match self {
            Volume::Android(android) => Some(android.conn.clone()),
            Volume::Sd => None,
        }
    }

    pub async fn rename(&self, from: &str, to: &str) -> Result<(), String> {
        match self {
            Volume::Android(android) => run(&android.conn, &alloc::format!("mv -- {} {}", shell_quote(from), shell_quote(to))).await,
            Volume::Sd => idf::fs::rename(&c_path(from)?, &c_path(to)?).map_err(|err| {
                error!(TAG, "rename {from} -> {to}: {err}");
                String::from("The item could not be renamed.")
            }),
        }
    }

    pub async fn create_dir(&self, path: &str) -> Result<(), String> {
        match self {
            Volume::Android(android) => run(&android.conn, &alloc::format!("mkdir -- {}", shell_quote(path))).await,
            Volume::Sd => idf::fs::create_dir(&c_path(path)?).map_err(|err| {
                error!(TAG, "mkdir {path}: {err}");
                String::from("The folder could not be created.")
            }),
        }
    }

    /// Size and modification time in seconds since 1970, following links.
    pub async fn stat(&self, path: &str) -> Result<(u64, i64), String> {
        match self {
            Volume::Android(android) => android.stat(path).await,
            Volume::Sd => {
                let meta = idf::fs::metadata(&c_path(path)?).map_err(|err| {
                    error!(TAG, "stat {path}: {err}");
                    String::from("The file could not be read.")
                })?;
                Ok((meta.len(), meta.modified()))
            }
        }
    }

    /// Up to `len` bytes from `offset`, fewer at the end of the file.
    pub async fn read_at(&self, path: &str, offset: u64, len: usize) -> Result<Vec<u8>, String> {
        match self {
            Volume::Android(android) => {
                let command = alloc::format!("tail -c +{} {} 2>/dev/null | head -c {len}", offset + 1, shell_quote(path));
                adb::services::exec(&android.conn, &command).await.map_err(|err| err.to_string())
            }
            Volume::Sd => read_at_sd(path, offset, len),
        }
    }

    pub async fn list(&self, path: &str) -> Result<Vec<Entry>, String> {
        let mut entries = match self {
            Volume::Android(android) => android.list(path).await?,
            Volume::Sd => list_sd(path)?,
        };
        sort(&mut entries);
        Ok(entries)
    }

    /// What `path`, a link, leads to; `None` where that cannot be told.
    pub async fn resolve(&self, path: &str) -> Option<Kind> {
        match self {
            Volume::Android(android) => android.resolve(path).await,
            Volume::Sd => None,
        }
    }
}

/// One `sync:` session shared by every clone, opened again after a failure.
#[derive(Clone)]
pub struct Android {
    conn: Connection,
    session: Arc<Mutex<Option<Sync>>>,
    stat_v2: bool,
}

impl Android {
    pub fn new(conn: Connection) -> Self {
        let stat_v2 = conn.has_feature(sync::FEATURE_STAT_V2);
        Self { conn, session: Arc::new(Mutex::new(None)), stat_v2 }
    }

    pub fn connection(&self) -> &Connection {
        &self.conn
    }

    async fn list(&self, path: &str) -> Result<Vec<Entry>, String> {
        let mut session = self.session.lock_async().await;
        let result = match open(&mut session, &self.conn).await {
            Ok(sync) => sync.list(path).await,
            Err(err) => Err(err),
        };
        let listed = result.inspect_err(|_| *session = None).map_err(|err| err.to_string())?;
        Ok(listed
            .into_iter()
            .filter(|e| e.name != "." && e.name != "..")
            .map(|e| {
                let kind = if e.stat.is_dir() {
                    Kind::Dir
                } else if e.stat.is_symlink() {
                    Kind::Link
                } else {
                    Kind::File
                };
                Entry { name: e.name, kind }
            })
            .collect())
    }

    async fn stat(&self, path: &str) -> Result<(u64, i64), String> {
        let mut session = self.session.lock_async().await;
        let result = match open(&mut session, &self.conn).await {
            Ok(sync) if self.stat_v2 => sync.stat_v2(path).await,
            Ok(sync) => sync.stat(path).await,
            Err(err) => Err(err),
        };
        match result {
            Ok(stat) if stat.exists() => Ok((stat.size, i64::from(stat.mtime))),
            Ok(_) => Err(String::from("The file no longer exists.")),
            Err(err) => {
                error!(TAG, "stat {path}: {err}");
                *session = None;
                Err(err.to_string())
            }
        }
    }

    async fn resolve(&self, path: &str) -> Option<Kind> {
        if !self.stat_v2 {
            return None;
        }
        let mut session = self.session.lock_async().await;
        let result = match open(&mut session, &self.conn).await {
            Ok(sync) => sync.stat_v2(path).await,
            Err(err) => Err(err),
        };
        match result {
            Ok(stat) if stat.is_dir() => Some(Kind::Dir),
            Ok(_) => Some(Kind::File),
            Err(err) => {
                error!(TAG, "stat {path}: {err}");
                *session = None;
                None
            }
        }
    }
}

async fn open<'a>(session: &'a mut Option<Sync>, conn: &Connection) -> adb::Result<&'a mut Sync> {
    if session.is_none() {
        *session = Some(Sync::open(conn).await?);
    }
    Ok(session.as_mut().unwrap())
}

pub fn sd_present() -> bool {
    SD.get().is_some()
}

/// Mounts the card when it is not.
pub fn sd_space() -> Result<Space, String> {
    mount_sd()?;
    let sd = SD.get().ok_or("no card slot")?;
    sd.space().map_err(|err| alloc::format!("{err}"))
}

fn mount_sd() -> Result<(), String> {
    let sd = SD.get().ok_or("no card slot")?;
    if sd.is_mounted() {
        return Ok(());
    }
    let mut config = MountConfig::default();
    config.psram_bounce_buffer = true;
    sd.mount(SD_ROOT_C, &config).map_err(|err| {
        error!(TAG, "mount: {err}");
        String::from("The SD card could not be mounted.")
    })
}

fn list_sd(path: &str) -> Result<Vec<Entry>, String> {
    mount_sd()?;
    let c_path = CString::new(path).map_err(|_| "bad path")?;
    let dir = idf::fs::read_dir(&c_path).map_err(|err| {
        error!(TAG, "read_dir {path}: {err}");
        if path == SD_ROOT && let Some(sd) = SD.get() {
            let _ = sd.unmount();
        }
        String::from("The folder could not be read.")
    })?;
    Ok(dir
        .map(|e| Entry { name: e.file_name().to_string_lossy().into_owned(), kind: if e.is_dir() { Kind::Dir } else { Kind::File } })
        .collect())
}

/// Free and total bytes of `df -k` output's first file system.
pub fn parse_df(out: &str) -> Option<Space> {
    let line = out.lines().find(|line| !line.starts_with("Filesystem") && !line.trim().is_empty())?;
    let fields: Vec<_> = line.split_whitespace().collect();
    let total: u64 = fields.get(1)?.parse().ok()?;
    let free: u64 = fields.get(3)?.parse().ok()?;
    Some(Space { total: total * 1024, free: free * 1024 })
}

pub fn format_bytes(bytes: u64) -> String {
    const UNITS: [&str; 4] = ["KB", "MB", "GB", "TB"];
    if bytes < 1000 {
        return alloc::format!("{bytes} B");
    }
    let mut unit = 0;
    let mut scale = 1000u64;
    while unit + 1 < UNITS.len() && bytes >= scale * 1000 {
        scale *= 1000;
        unit += 1;
    }
    let tenths = (bytes * 10 + scale / 2) / scale;
    alloc::format!("{}.{} {}", tenths / 10, tenths % 10, UNITS[unit])
}

pub fn c_path(path: &str) -> Result<CString, String> {
    CString::new(path).map_err(|_| String::from("The name is not valid."))
}

pub fn shell_quote(text: &str) -> String {
    alloc::format!("'{}'", text.replace('\'', "'\\''"))
}

/// Runs `command` on the device; its output becomes the error when it exits
/// with a status other than 0.
pub async fn run(conn: &Connection, command: &str) -> Result<(), String> {
    let out = adb::services::exec(conn, &alloc::format!("{command} 2>&1; echo \":$?\"")).await.map_err(|err| {
        error!(TAG, "{command}: {err}");
        err.to_string()
    })?;
    let out = String::from_utf8_lossy(&out);
    let (message, status) = out.trim_end().rsplit_once(':').unwrap_or(("", "1"));
    if status == "0" {
        return Ok(());
    }
    let message = message.trim();
    error!(TAG, "{command}: {message} ({status})");
    Err(if message.is_empty() { alloc::format!("exit status {status}") } else { message.to_string() })
}

pub fn name_of(path: &str) -> &str {
    path.rsplit('/').next().unwrap_or(path)
}

/// `path` is `ancestor` or inside it.
pub fn is_within(path: &str, ancestor: &str) -> bool {
    path.strip_prefix(ancestor).is_some_and(|rest| rest.is_empty() || rest.starts_with('/'))
}

const INVALID_NAME_CHARS: &[char] = &['/', '\\', ':', '*', '?', '"', '<', '>', '|'];

pub fn validate_name(name: &str) -> Result<(), &'static str> {
    if name.is_empty() {
        Err("Enter a name.")
    } else if name == "." || name == ".." {
        Err("This name is not allowed.")
    } else if name.contains(INVALID_NAME_CHARS) {
        Err("A name can't contain / \\ : * ? \" < > |")
    } else {
        Ok(())
    }
}

/// Where the extension of `name` starts, if a file's name has one.
pub fn extension_at(name: &str, folder: bool) -> Option<usize> {
    if folder {
        return None;
    }
    name.rfind('.').filter(|&dot| dot > 0)
}

/// `name`, or `name (2)`, `name (3)`... before the extension, whichever
/// `taken` does not hold.
pub fn unused_name(name: &str, folder: bool, taken: impl Fn(&str) -> bool) -> String {
    if !taken(name) {
        return name.to_string();
    }
    let (stem, extension) = extension_at(name, folder).map_or((name, ""), |dot| name.split_at(dot));
    (2..).map(|n| alloc::format!("{stem} ({n}){extension}")).find(|candidate| !taken(candidate)).unwrap()
}

/// A file opened from a browser.
#[derive(Clone)]
pub struct FileRef {
    pub volume: Volume,
    /// The storage as the browser names it.
    pub label: &'static str,
    pub path: String,
}

impl FileRef {
    pub fn name(&self) -> &str {
        name_of(&self.path)
    }
}

fn read_at_sd(path: &str, offset: u64, len: usize) -> Result<Vec<u8>, String> {
    let file = idf::fs::File::open(&c_path(path)?).map_err(|err| alloc::format!("open: {err}"))?;
    file.seek(offset).map_err(|err| alloc::format!("seek: {err}"))?;
    let mut out = alloc::vec![0u8; len];
    let mut filled = 0;
    while filled < len {
        match file.read(&mut out[filled..]) {
            Ok(0) => break,
            Ok(n) => filled += n,
            Err(err) => return Err(alloc::format!("read: {err}")),
        }
    }
    out.truncate(filled);
    Ok(out)
}

pub fn type_label(file_type: FileType) -> &'static str {
    match file_type {
        FileType::Apk => "Android package",
        FileType::Image => "Image",
        FileType::Video => "Video",
        FileType::Audio => "Audio",
        FileType::Text => "Text file",
        FileType::Other => "File",
    }
}

/// `YYYY-MM-DD HH:MM` in UTC.
pub fn format_date_time(secs: i64) -> String {
    let (days, rest) = (secs.div_euclid(86400), secs.rem_euclid(86400));
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    alloc::format!("{year:04}-{month:02}-{day:02} {:02}:{:02}", rest / 3600, rest % 3600 / 60)
}
