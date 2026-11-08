/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::{String, ToString};
use alloc::vec::Vec;

const SEPARATOR: &str = "---SEP---";

pub const COMMAND: &str = "settings get global device_name; echo ---SEP---; \
    getprop ro.build.version.release; echo ---SEP---; \
    dumpsys battery; echo ---SEP---; \
    df -k /data; echo ---SEP---; \
    cmd wifi status";

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Battery {
    pub level: u8,
    pub charging: bool,
}

#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct DeviceInfo {
    pub name: Option<String>,
    pub version: String,
    pub storage_kb: Option<u64>,
    pub battery: Option<Battery>,
    pub wifi: bool,
}

pub fn parse(out: &str) -> DeviceInfo {
    let mut sections = out.split(SEPARATOR);
    let mut next = || sections.next().unwrap_or("");
    let name = first_line(next());
    let version = first_line(next());
    DeviceInfo {
        name: (!name.is_empty() && name != "null").then(|| name.to_string()),
        version: version.to_string(),
        battery: battery(next()),
        storage_kb: storage_kb(next()),
        wifi: next().contains("Wifi is connected to"),
    }
}

/// `ro.product.model` from the banner the device sent with its CNXN.
pub fn model(banner: &str) -> Option<String> {
    let body = banner.split_once("::").map_or(banner, |(_, body)| body);
    body.split(';').find_map(|field| field.strip_prefix("ro.product.model=")).filter(|m| !m.is_empty()).map(ToString::to_string)
}

fn first_line(section: &str) -> &str {
    section.trim_start_matches(['\r', '\n']).lines().next().unwrap_or("").trim()
}

fn battery(section: &str) -> Option<Battery> {
    let (mut level, mut status) = (None, 0);
    for line in section.lines().map(str::trim) {
        if let Some(value) = line.strip_prefix("level:") {
            level = value.trim().parse::<u8>().ok().filter(|&l| l <= 100);
        } else if let Some(value) = line.strip_prefix("status:") {
            status = value.trim().parse().unwrap_or(0);
        }
    }
    level.map(|level| Battery { level, charging: status == 2 })
}

fn storage_kb(section: &str) -> Option<u64> {
    section
        .lines()
        .filter(|line| !line.starts_with("Filesystem"))
        .find_map(|line| line.split_whitespace().nth(1)?.parse().ok().filter(|&total: &u64| total > 0))
}

pub const HARDWARE_COMMAND: &str = "getprop; echo ---SEP---; \
    wm size; wm density; echo ---SEP---; \
    cat /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq 2>/dev/null | sort -n | tail -1; echo ---SEP---; \
    ls /sys/devices/system/cpu/ | grep -cE '^cpu[0-9]+$'; echo ---SEP---; \
    cat /proc/version";

pub const STATUS_COMMAND: &str = "grep -E 'MemTotal|MemAvailable' /proc/meminfo; echo ---SEP---; \
    df -k /data; echo ---SEP---; \
    dumpsys battery; echo ---SEP---; \
    cmd wifi status; echo ---SEP---; \
    getprop gsm.sim.state; echo ---SEP---; \
    dumpsys telephony.registry | grep -E 'mServiceState=|mSignalStrength=' | head -4; echo ---SEP---; \
    cat /proc/uptime";

pub const METRICS_MARKER: &str = "@@@";

pub const METRICS_COMMAND: &str = "while true; do echo @@@; \
    grep ^cpu /proc/stat; \
    grep -E 'MemTotal|MemAvailable' /proc/meminfo; \
    cat /proc/loadavg; \
    dumpsys battery | grep -E 'level:|temperature:'; \
    sleep 1; done";

#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct Hardware {
    pub props: Vec<(String, String)>,
    pub display: Option<String>,
    pub max_freq_khz: Option<u32>,
    pub cores: Option<u32>,
    pub kernel: Option<String>,
}

impl Hardware {
    /// Empty when the device does not set `key`.
    pub fn prop(&self, key: &str) -> &str {
        self.props.iter().find(|(k, _)| k == key).map_or("", |(_, v)| v.as_str())
    }

    pub fn soc(&self) -> &str {
        ["ro.soc.model", "ro.board.platform", "ro.hardware"].into_iter().map(|key| self.prop(key)).find(|v| !v.is_empty()).unwrap_or("")
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Memory {
    pub total_kb: u64,
    pub available_kb: u64,
}

impl Memory {
    pub fn used_kb(&self) -> u64 {
        self.total_kb.saturating_sub(self.available_kb)
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Storage {
    pub total_kb: u64,
    pub used_kb: u64,
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub struct BatteryState {
    pub level: u8,
    pub status: u8,
    pub health: u8,
    pub voltage_mv: u32,
    pub temperature_dc: i32,
    pub technology: String,
}

impl BatteryState {
    pub fn status_text(&self) -> &'static str {
        match self.status {
            2 => "Charging",
            3 => "Discharging",
            4 => "Not charging",
            5 => "Full",
            _ => "Unknown",
        }
    }

    pub fn health_text(&self) -> &'static str {
        match self.health {
            2 => "Good",
            3 => "Overheat",
            4 => "Dead",
            5 => "Over voltage",
            6 => "Failure",
            7 => "Cold",
            _ => "Unknown",
        }
    }
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Wifi {
    Off,
    Disconnected,
    Connected { rssi: i32, ip: Option<String> },
}

impl Wifi {
    pub fn bars(rssi: i32) -> u8 {
        match rssi {
            -55.. => 4,
            -67.. => 3,
            -80.. => 2,
            _ => 1,
        }
    }
}

#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct Cellular {
    pub sim_state: String,
    pub in_service: bool,
    pub level: u8,
}

impl Cellular {
    pub fn sim_present(&self) -> bool {
        self.sim_state.split(',').any(|s| matches!(s.trim(), "READY" | "LOADED"))
    }
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Status {
    pub memory: Option<Memory>,
    pub storage: Option<Storage>,
    pub battery: Option<BatteryState>,
    pub wifi: Wifi,
    pub cellular: Cellular,
    pub uptime_s: Option<u64>,
}

/// One `/proc/stat` sample: the aggregate first, then each core, as
/// `(busy, total)` jiffies.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct CpuTimes(pub Vec<(u64, u64)>);

#[derive(Clone, PartialEq, Debug, Default)]
pub struct MetricsSample {
    pub cpu: CpuTimes,
    pub memory: Option<Memory>,
    pub load: Option<String>,
    pub battery_level: Option<u8>,
    pub temperature_dc: Option<i32>,
}

pub fn parse_hardware(out: &str) -> Hardware {
    let mut sections = out.split(SEPARATOR);
    let mut next = || sections.next().unwrap_or("");
    let props = getprop(next());
    let display = display(next());
    let max_freq_khz = first_line(next()).parse().ok().filter(|&f| f > 0);
    let cores = first_line(next()).parse().ok().filter(|&c| c > 0);
    let kernel = kernel(next());
    Hardware { props, display, max_freq_khz, cores, kernel }
}

pub fn parse_status(out: &str) -> Status {
    let mut sections = out.split(SEPARATOR);
    let mut next = || sections.next().unwrap_or("");
    let memory = memory(next());
    let storage = storage(next());
    let battery = battery_state(next());
    let wifi = wifi(next());
    let sim_state = first_line(next()).to_string();
    let cellular = cellular(sim_state, next());
    let uptime_s = first_line(next()).split(['.', ' ']).next().and_then(|s| s.parse().ok());
    Status { memory, storage, battery, wifi, cellular, uptime_s }
}

pub fn parse_metrics(frame: &str) -> MetricsSample {
    let mut sample = MetricsSample { memory: memory(frame), ..Default::default() };
    for line in frame.lines().map(str::trim) {
        if let Some(rest) = line.strip_prefix("cpu") {
            if let Some(times) = cpu_times(rest) {
                sample.cpu.0.push(times);
            }
        } else if let Some(value) = line.strip_prefix("level:") {
            sample.battery_level = value.trim().parse().ok().filter(|&l| l <= 100);
        } else if let Some(value) = line.strip_prefix("temperature:") {
            sample.temperature_dc = value.trim().parse().ok();
        } else if line.starts_with(|c: char| c.is_ascii_digit()) && line.contains('/') {
            let load: Vec<_> = line.split_whitespace().take(3).collect();
            sample.load = (load.len() == 3).then(|| load.join(" "));
        }
    }
    sample
}

pub fn cpu_usage(prev: &CpuTimes, now: &CpuTimes) -> Option<Vec<u8>> {
    if prev.0.is_empty() || prev.0.len() != now.0.len() {
        return None;
    }
    let usage = prev.0.iter().zip(&now.0).map(|(&(busy0, total0), &(busy1, total1))| {
        let total = total1.saturating_sub(total0);
        let busy = busy1.saturating_sub(busy0).min(total);
        if total == 0 { 0 } else { ((busy * 100 + total / 2) / total) as u8 }
    });
    Some(usage.collect())
}

fn getprop(section: &str) -> Vec<(String, String)> {
    section
        .lines()
        .filter_map(|line| {
            let (key, value) = line.trim().strip_prefix('[')?.split_once("]: [")?;
            Some((key.to_string(), value.strip_suffix(']')?.to_string()))
        })
        .collect()
}

fn wm_value<'a>(section: &'a str, kind: &str) -> Option<&'a str> {
    ["Override", "Physical"].into_iter().find_map(|prefix| {
        section.lines().find_map(|line| line.trim().strip_prefix(prefix)?.trim_start().strip_prefix(kind)?.strip_prefix(": ")).map(str::trim).filter(|v| !v.is_empty())
    })
}

fn display(section: &str) -> Option<String> {
    let size = wm_value(section, "size")?;
    Some(match wm_value(section, "density") {
        Some(density) => alloc::format!("{size} @ {density} dpi"),
        None => size.to_string(),
    })
}

fn kernel(section: &str) -> Option<String> {
    let mut words = first_line(section).split_whitespace();
    (words.next() == Some("Linux") && words.next() == Some("version")).then(|| words.next()).flatten().map(ToString::to_string)
}

fn kb_after(section: &str, key: &str) -> Option<u64> {
    section.lines().find_map(|line| line.trim().strip_prefix(key)?.split_whitespace().next()?.parse().ok())
}

fn memory(section: &str) -> Option<Memory> {
    let total_kb = kb_after(section, "MemTotal:").filter(|&t| t > 0)?;
    let available_kb = kb_after(section, "MemAvailable:")?.min(total_kb);
    Some(Memory { total_kb, available_kb })
}

fn storage(section: &str) -> Option<Storage> {
    section.lines().filter(|line| !line.starts_with("Filesystem")).find_map(|line| {
        let mut fields = line.split_whitespace().skip(1).map(|f| f.parse::<u64>().ok());
        let (total_kb, used_kb) = (fields.next()??, fields.next()??);
        (total_kb > 0).then_some(Storage { total_kb, used_kb })
    })
}

fn battery_state(section: &str) -> Option<BatteryState> {
    let mut level = None;
    let mut state = BatteryState { level: 0, status: 0, health: 0, voltage_mv: 0, temperature_dc: 0, technology: String::new() };
    for line in section.lines().map(str::trim) {
        let Some((key, value)) = line.split_once(':') else { continue };
        let value = value.trim();
        match key {
            "level" => level = value.parse::<u8>().ok().filter(|&l| l <= 100),
            "status" => state.status = value.parse().unwrap_or(0),
            "health" => state.health = value.parse().unwrap_or(0),
            "voltage" => state.voltage_mv = value.parse().unwrap_or(0),
            "temperature" => state.temperature_dc = value.parse().unwrap_or(0),
            "technology" => state.technology = value.to_string(),
            _ => {}
        }
    }
    state.level = level?;
    Some(state)
}

fn wifi(section: &str) -> Wifi {
    if section.contains("Wifi is disabled") {
        return Wifi::Off;
    }
    if !section.contains("Wifi is connected to") {
        return Wifi::Disconnected;
    }
    let rssi = section.split_once("RSSI:").and_then(|(_, rest)| rest.trim_start().split(|c: char| c != '-' && !c.is_ascii_digit()).next()?.parse().ok()).unwrap_or(0);
    let ip = section.split_once("IP: /").map(|(_, rest)| rest.split([',', ' ', '\r', '\n']).next().unwrap_or("").to_string()).filter(|ip| !ip.is_empty());
    Wifi::Connected { rssi, ip }
}

/// `level` of the strongest radio; the NR block writes `level = N`.
fn cellular(sim_state: String, registry: &str) -> Cellular {
    let in_service = registry.contains("mVoiceRegState=0") || registry.contains("mDataRegState=0");
    let level = registry
        .match_indices("level")
        .filter_map(|(at, word)| {
            let rest = registry[at + word.len()..].trim_start().strip_prefix('=')?.trim_start();
            let digits = rest.find(|c: char| !c.is_ascii_digit()).unwrap_or(rest.len());
            rest[..digits].parse::<u8>().ok().filter(|&l| l <= 4)
        })
        .max()
        .unwrap_or(0);
    Cellular { sim_state, in_service, level }
}

fn cpu_times(rest: &str) -> Option<(u64, u64)> {
    let fields = if rest.starts_with(char::is_whitespace) {
        rest
    } else {
        let (index, fields) = rest.split_once(char::is_whitespace)?;
        if !index.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        fields
    };
    let fields: Vec<u64> = fields.split_whitespace().take(10).map_while(|w| w.parse().ok()).collect();
    if fields.len() < 4 {
        return None;
    }
    let total: u64 = fields.iter().sum();
    let idle = fields[3] + fields.get(4).copied().unwrap_or(0);
    Some((total - idle, total))
}

pub fn format_kb(kb: u64) -> String {
    if kb >= 1024 * 1024 {
        let tenths = (kb * 10 + 512 * 1024) / (1024 * 1024);
        alloc::format!("{}.{} GB", tenths / 10, tenths % 10)
    } else if kb >= 1024 {
        alloc::format!("{} MB", (kb + 512) / 1024)
    } else {
        alloc::format!("{kb} kB")
    }
}

const STORAGE_SIZES_GB: [u64; 10] = [4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048];
const RAM_SIZES_GB: [u64; 12] = [1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64];

fn marketed_gb(total_kb: u64, sizes: &[u64]) -> Option<u64> {
    let gib_ceil = total_kb.div_ceil(1024 * 1024);
    (total_kb > 0).then(|| sizes.iter().copied().find(|&s| gib_ceil <= s).unwrap_or(gib_ceil))
}

pub fn marketed_storage_gb(total_kb: u64) -> Option<u64> {
    marketed_gb(total_kb, &STORAGE_SIZES_GB)
}

pub fn marketed_ram_gb(total_kb: u64) -> Option<u64> {
    marketed_gb(total_kb, &RAM_SIZES_GB)
}

pub fn format_temperature(dc: i32) -> String {
    let sign = if dc < 0 { "-" } else { "" };
    alloc::format!("{sign}{}.{}\u{b0}C", dc.unsigned_abs() / 10, dc.unsigned_abs() % 10)
}

pub fn format_uptime(seconds: u64) -> String {
    let (d, h, m) = (seconds / 86400, seconds % 86400 / 3600, seconds % 3600 / 60);
    match (d, h) {
        (0, 0) => alloc::format!("{m} m"),
        (0, _) => alloc::format!("{h} h {m} m"),
        _ => alloc::format!("{d} d {h} h {m} m"),
    }
}

pub fn format_frequency(khz: u32) -> String {
    let centi = (khz + 5000) / 10000;
    alloc::format!("{}.{:02} GHz", centi / 100, centi % 100)
}

pub fn format_build_date(date: &str) -> String {
    let words: Vec<_> = date.split_whitespace().collect();
    match words.as_slice() {
        [weekday, month, day, _, _, year] => alloc::format!("{weekday} {month} {day} {year}"),
        _ => date.to_string(),
    }
}
