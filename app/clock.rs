/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::String;
use core::ffi::CStr;

use adb::link::Connection;
use bsp_rs::rtc::DateTime;
use idf::log::{error, info};

use crate::settings;

const TAG: &CStr = c"clock";
/// 2024-01-01 UTC; a system time before it has not been set.
const SET_SINCE: i64 = 1_704_067_200;
const SYNC_TOLERANCE: i64 = 2;

pub fn init() {
    if let Err(err) = idf::time::set_utc_offset(i32::from(settings::utc_offset())) {
        error!(TAG, "set_utc_offset: {err}");
    }
}

/// Seconds since 1970 UTC; `None` while the time has not been set.
pub fn unix_time() -> Option<i64> {
    Some(idf::time::unix_time()).filter(|&t| t >= SET_SINCE)
}

pub fn local_now() -> Option<DateTime> {
    unix_time().map(|t| DateTime::from_seconds(t + i64::from(idf::time::utc_offset()) * 60))
}

/// Takes the time and zone from the device of `conn`.
pub async fn sync(conn: &Connection) {
    let out = match adb::services::exec(conn, "date +'%s %z'").await {
        Ok(out) => out,
        Err(err) => {
            error!(TAG, "date: {err}");
            return;
        }
    };
    let text = String::from_utf8_lossy(&out);
    let Some((seconds, offset)) = parse_date(&text) else {
        error!(TAG, "date: unexpected {:?}", text.trim());
        return;
    };
    if (idf::time::unix_time() - seconds).abs() > SYNC_TOLERANCE {
        match bsp_rs::rtc::sync(seconds) {
            Ok(()) => info!(TAG, "set to {seconds}"),
            Err(err) => error!(TAG, "rtc sync: {err}"),
        }
    }
    if i32::from(offset) != idf::time::utc_offset() {
        if let Err(err) = idf::time::set_utc_offset(i32::from(offset)) {
            error!(TAG, "set_utc_offset: {err}");
            return;
        }
        settings::set_utc_offset(offset);
        settings::commit();
    }
}

/// `date +'%s %z'` output: seconds since 1970 UTC and minutes east of UTC.
pub fn parse_date(out: &str) -> Option<(i64, i16)> {
    let mut fields = out.split_whitespace();
    let seconds: i64 = fields.next()?.parse().ok()?;
    let zone = fields.next()?;
    let (sign, digits) = match zone.as_bytes().first()? {
        b'+' => (1, &zone[1..]),
        b'-' => (-1, &zone[1..]),
        _ => return None,
    };
    if seconds <= 0 || digits.len() != 4 || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    let (hours, minutes): (i16, i16) = (digits[..2].parse().ok()?, digits[2..].parse().ok()?);
    if hours > 14 || minutes >= 60 {
        return None;
    }
    Some((seconds, sign * (hours * 60 + minutes)))
}

/// "YYYYMMDD_HHMMSS" in local time, for file names.
pub fn file_stamp() -> Option<String> {
    let t = local_now()?;
    Some(alloc::format!("{:04}{:02}{:02}_{:02}{:02}{:02}", t.year, t.month, t.day, t.hour, t.minute, t.second))
}
