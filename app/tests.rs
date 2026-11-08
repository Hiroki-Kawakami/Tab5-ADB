/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use agent_link::FrameHead;
use devkit_gfx::{Rect, Rotation};

use crate::device_info::{self, Battery};
use crate::screens::mirror::{Placed, strip_rect};

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

#[test]
fn a_touch_finds_the_frame_pixel_under_it_however_the_frame_is_turned() {
    let (width, height) = (40u16, 64u16);
    for turn in [Rotation::R0, Rotation::R90, Rotation::R180, Rotation::R270] {
        let (w, h) = if turn.swaps_axes() { (height, width) } else { (width, height) };
        let rect = Rect::new(100, 200, w as i16, h as i16);
        let placed = Placed { rect, turn, width, height, rotation: 0 };
        let band = FrameHead { width, height, y: 16, rows: 16, rotation: 0, last: false };
        let band = strip_rect(&band, rect, turn);
        let mut seen = alloc::vec::Vec::new();
        for y in band.y..band.bottom() {
            for x in band.x..band.right() {
                let (fx, fy) = placed.to_frame(x, y);
                assert!((0..i32::from(width)).contains(&fx) && (16..32).contains(&fy), "{turn:?} ({x}, {y}) -> ({fx}, {fy})");
                seen.push((fx, fy));
            }
        }
        seen.sort_unstable();
        seen.dedup();
        assert_eq!(seen.len(), usize::from(width) * 16, "{turn:?}");
    }
}
