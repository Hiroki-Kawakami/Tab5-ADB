/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use crate::device_info::{self, Battery};

const SUMMARY: &str = "Example Phone\n---SEP---\n15\n---SEP---\nCurrent Battery Service state:\n  AC powered: false\n  USB powered: true\n  status: 2\n  health: 2\n  level: 64\n  scale: 100\n---SEP---\nFilesystem     1K-blocks     Used Available Use% Mounted on\n/dev/block/dm-48 115343360 41234567 74108793  36% /data\n---SEP---\nWifi is enabled\nWifi scanning is always available\nWifi is connected to \"example-ap\"\n";

#[test]
fn device_summary() {
    let info = device_info::parse(SUMMARY);
    assert_eq!(info.name.as_deref(), Some("Example Phone"));
    assert_eq!(info.version, "15");
    assert_eq!(info.battery, Some(Battery { level: 64, charging: true }));
    assert_eq!(info.storage_kb, Some(115343360));
    assert!(info.wifi);
}

#[test]
fn device_summary_unset() {
    let info = device_info::parse("null\n---SEP---\n14\n---SEP---\n  status: 3\n---SEP---\n---SEP---\nWifi is disabled\n");
    assert_eq!(info.name, None);
    assert_eq!(info.battery, None);
    assert_eq!(info.storage_kb, None);
    assert!(!info.wifi);
}

#[test]
fn banner_model() {
    assert_eq!(device_info::model("device::ro.product.name=example;ro.product.model=Example Phone;features=shell_v2").as_deref(), Some("Example Phone"));
    assert_eq!(device_info::model("device::features=shell_v2"), None);
}
