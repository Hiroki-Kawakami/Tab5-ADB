/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::{String, ToString};

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
