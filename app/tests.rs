/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use agent_link::FrameHead;
use devkit_gfx::{Rect, Rotation};

use crate::device_info::{self, Battery, Cellular, CpuTimes, Memory, Storage, Wifi};
use core::future::Future;

use crate::apk;
use crate::apps::{self, Action};
use crate::clock;
use crate::logcat::{Filter, Level, LineSplitter, Log};
use crate::files::{self, Entry, FileType, Kind};
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

const HARDWARE: &str = "[ro.product.model]: [Example Phone]\n[ro.product.manufacturer]: [Example]\n[ro.soc.manufacturer]: [ExampleSoC]\n[ro.soc.model]: [X100]\n[ro.build.date]: [Mon Oct  5 12:34:56 UTC 2026]\n[empty]: []\n---SEP---\nPhysical size: 1080x2400\nOverride size: 720x1600\nPhysical density: 420\n---SEP---\n3000000\n---SEP---\n8\n---SEP---\nLinux version 6.1.0-example (builder@host) (clang) #1 SMP PREEMPT\n";

#[test]
fn device_hardware() {
    let hw = device_info::parse_hardware(HARDWARE);
    assert_eq!(hw.prop("ro.product.model"), "Example Phone");
    assert_eq!(hw.prop("empty"), "");
    assert_eq!(hw.prop("missing"), "");
    assert_eq!(hw.soc(), "X100");
    assert_eq!(hw.display.as_deref(), Some("720x1600 @ 420 dpi"));
    assert_eq!(hw.max_freq_khz, Some(3000000));
    assert_eq!(hw.cores, Some(8));
    assert_eq!(hw.kernel.as_deref(), Some("6.1.0-example"));
    assert_eq!(device_info::format_build_date(hw.prop("ro.build.date")), "Mon Oct 5 2026");
}

const STATUS: &str = "MemTotal:       11849728 kB\nMemAvailable:    4823449 kB\n---SEP---\nFilesystem     1K-blocks     Used Available Use% Mounted on\n/dev/block/dm-48 114609152 86507520 28101632  76% /data\n---SEP---\nCurrent Battery Service state:\n  AC powered: false\n  status: 2\n  health: 2\n  level: 22\n  voltage: 3855\n  temperature: 254\n  technology: Li-ion\n---SEP---\nWifi is enabled\nWifi is connected to \"example-ap\"\nWifiInfo: SSID: \"example-ap\", RSSI: -51, Link speed: 866Mbps, IP: /192.168.x.x, Frequency: 5180MHz\n---SEP---\nLOADED\n---SEP---\n  mServiceState={mVoiceRegState=0(IN_SERVICE), mDataRegState=0(IN_SERVICE)}\n  mSignalStrength=SignalStrength:{mLte=CellSignalStrengthLte: rssi=-61 level=3,mNr=CellSignalStrengthNr:{ level = 2 },mGsm=CellSignalStrengthGsm: level=2147483647}\n---SEP---\n273120.55 1987654.32\n";

#[test]
fn device_status() {
    let status = device_info::parse_status(STATUS);
    assert_eq!(status.memory, Some(Memory { total_kb: 11849728, available_kb: 4823449 }));
    assert_eq!(status.storage, Some(Storage { total_kb: 114609152, used_kb: 86507520 }));
    let battery = status.battery.unwrap();
    assert_eq!((battery.level, battery.status_text(), battery.health_text(), battery.voltage_mv), (22, "Charging", "Good", 3855));
    assert_eq!(device_info::format_temperature(battery.temperature_dc), "25.4\u{b0}C");
    assert_eq!(battery.technology, "Li-ion");
    assert_eq!(status.wifi, Wifi::Connected { rssi: -51, ip: Some("192.168.x.x".into()) });
    assert_eq!(Wifi::bars(-51), 4);
    assert_eq!(status.cellular, Cellular { sim_state: "LOADED".into(), in_service: true, level: 3 });
    assert!(status.cellular.sim_present());
    assert_eq!(device_info::format_uptime(status.uptime_s.unwrap()), "3 d 3 h 52 m");
}

#[test]
fn device_status_unset() {
    let status = device_info::parse_status("---SEP---\n---SEP---\n---SEP---\nWifi is disabled\n---SEP---\nABSENT\n---SEP---\n---SEP---\n");
    assert_eq!((status.memory, status.storage, status.battery, status.uptime_s), (None, None, None, None));
    assert_eq!(status.wifi, Wifi::Off);
    assert!(!status.cellular.sim_present());
}

#[test]
fn device_sizes() {
    assert_eq!(device_info::format_kb(114609152), "109.3 GB");
    assert_eq!(device_info::format_kb(524288), "512 MB");
    assert_eq!(device_info::marketed_storage_gb(114609152), Some(128));
    assert_eq!(device_info::marketed_ram_gb(11849728), Some(12));
    assert_eq!(device_info::marketed_ram_gb(0), None);
    assert_eq!(device_info::format_frequency(3782000), "3.78 GHz");
    assert_eq!(device_info::format_uptime(59), "0 m");
}

#[test]
fn device_metrics() {
    let first = device_info::parse_metrics("cpu  100 0 100 800 0 0 0 0 0 0\ncpu0 50 0 50 400 0 0 0\ncpu1 50 0 50 400 0 0 0\nMemTotal: 1000 kB\nMemAvailable: 400 kB\n3.98 5.02 5.68 2/3733 6728\n  level: 22\n  temperature: 254\n");
    assert_eq!(first.cpu, CpuTimes(alloc::vec![(200, 1000), (100, 500), (100, 500)]));
    assert_eq!(first.memory.map(|m| m.used_kb()), Some(600));
    assert_eq!(first.load.as_deref(), Some("3.98 5.02 5.68"));
    assert_eq!((first.battery_level, first.temperature_dc), (Some(22), Some(254)));
    let second = device_info::parse_metrics("cpu  150 0 150 900 0 0 0 0 0 0\ncpu0 100 0 100 400 0 0 0\ncpu1 50 0 50 500 0 0 0\n");
    assert_eq!(device_info::cpu_usage(&first.cpu, &second.cpu), Some(alloc::vec![50, 100, 0]));
    assert_eq!(device_info::cpu_usage(&CpuTimes::default(), &second.cpu), None);
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

fn entry(name: &str, kind: Kind) -> Entry {
    Entry { name: name.into(), kind }
}

#[test]
fn folders_and_links_sort_first() {
    let mut entries = [entry("b.txt", Kind::File), entry("Zeta", Kind::Dir), entry("A.txt", Kind::File), entry("etc", Kind::Link), entry("alpha", Kind::Dir)];
    files::sort(&mut entries);
    let names: alloc::vec::Vec<_> = entries.iter().map(|e| e.name.as_str()).collect();
    assert_eq!(names, ["alpha", "etc", "Zeta", "A.txt", "b.txt"]);
}

#[test]
fn file_types_by_extension() {
    assert_eq!(files::file_type("app-release.APK"), FileType::Apk);
    assert_eq!(files::file_type("IMG_0001.jpg"), FileType::Image);
    assert_eq!(files::file_type("clip.mp4"), FileType::Video);
    assert_eq!(files::file_type("episode.mp3"), FileType::Audio);
    assert_eq!(files::file_type("notes.txt"), FileType::Text);
    assert_eq!(files::file_type("firmware.bin"), FileType::Other);
    assert_eq!(files::file_type("Makefile"), FileType::Other);
}

#[test]
fn paths_join() {
    assert_eq!(files::join("/", "sdcard"), "/sdcard");
    assert_eq!(files::join("/sdcard", "Download"), "/sdcard/Download");
}

#[test]
fn df_space() {
    let out = "Filesystem     1K-blocks     Used Available Use% Mounted on\n/dev/fuse       115343360 41234567  74108793  36% /storage/emulated\n";
    let space = files::parse_df(out).unwrap();
    assert_eq!((space.total, space.free), (115343360 * 1024, 74108793 * 1024));
    assert!(files::parse_df("df: /sdcard: Permission denied\n").is_none());
}

#[test]
fn byte_sizes() {
    assert_eq!(files::format_bytes(512), "512 B");
    assert_eq!(files::format_bytes(1_500), "1.5 KB");
    assert_eq!(files::format_bytes(26_600_000_000), "26.6 GB");
    assert_eq!(files::format_bytes(29_700_000_000), "29.7 GB");
}

#[test]
fn unused_names() {
    let taken = |names: &'static [&'static str]| move |name: &str| names.contains(&name);
    assert_eq!(files::unused_name("a.txt", false, taken(&[])), "a.txt");
    assert_eq!(files::unused_name("a.txt", false, taken(&["a.txt"])), "a (2).txt");
    assert_eq!(files::unused_name("a.txt", false, taken(&["a.txt", "a (2).txt"])), "a (3).txt");
    assert_eq!(files::unused_name("archive.v1", true, taken(&["archive.v1"])), "archive.v1 (2)");
    assert_eq!(files::unused_name(".profile", false, taken(&[".profile"])), ".profile (2)");
}

#[test]
fn names_checked() {
    assert!(files::validate_name("notes.txt").is_ok());
    assert!(files::validate_name("").is_err());
    assert!(files::validate_name("..").is_err());
    assert!(files::validate_name("a/b").is_err());
    assert!(files::validate_name("what?").is_err());
}

#[test]
fn paths_within() {
    assert!(files::is_within("/sdcard/A", "/sdcard/A"));
    assert!(files::is_within("/sdcard/A/b", "/sdcard/A"));
    assert!(!files::is_within("/sdcard/AB", "/sdcard/A"));
    assert!(!files::is_within("/sdcard", "/sdcard/A"));
}

#[test]
fn shell_quoting() {
    assert_eq!(files::shell_quote("a b"), "'a b'");
    assert_eq!(files::shell_quote("it's"), "'it'\\''s'");
}

struct Bytes(&'static [u8]);

impl apk::ReadAt for Bytes {
    fn len(&self) -> u64 {
        self.0.len() as u64
    }

    async fn read_at(&mut self, offset: u64, len: usize) -> Result<alloc::vec::Vec<u8>, alloc::string::String> {
        let start = (offset as usize).min(self.0.len());
        Ok(self.0[start..(start + len).min(self.0.len())].to_vec())
    }
}

fn read_apk(bytes: &'static [u8]) -> Result<apk::ApkInfo, alloc::string::String> {
    let mut future = core::pin::pin!(async { apk::read(&mut Bytes(bytes)).await });
    match future.as_mut().poll(&mut core::task::Context::from_waker(core::task::Waker::noop())) {
        core::task::Poll::Ready(result) => result,
        core::task::Poll::Pending => panic!("a slice never waits"),
    }
}

#[test]
fn apk_with_a_literal_label() {
    let info = read_apk(include_bytes!("../simulator/sdcard/testapp.apk")).unwrap();
    assert_eq!(info.package, "com.tab5adb.testapp");
    assert_eq!(info.label, "Tab5 Test App");
    assert!(info.icon.is_none());
    assert_eq!(info.version(), "1.0 (1)");
    assert_eq!(info.sdk(), "min 21 / target 34");
}

#[test]
fn apk_label_from_its_resources() {
    let info = read_apk(include_bytes!("../simulator/sdcard/resapp.apk")).unwrap();
    assert_eq!(info.package, "com.tab5adb.resapp");
    assert_eq!(info.label, "Resource Label App");
    assert_eq!(info.version(), "2.3.1 (7)");
    assert_eq!(info.sdk(), "min 24 / target 35");
    let icon = info.icon.expect("the densest bitmap icon");
    assert_eq!((&icon[..4], icon.len()), (&b"\x89PNG"[..], 1672));
}

#[test]
fn not_an_apk() {
    assert!(read_apk(include_bytes!("../simulator/sdcard/dummy.apk")).is_err());
}

#[test]
fn date_times() {
    assert_eq!(files::format_date_time(0), "1970-01-01 00:00");
    assert_eq!(files::format_date_time(1_791_571_920), "2026-10-09 18:52");
}

#[test]
fn apk_label_from_compact_resources() {
    let info = read_apk(include_bytes!("../simulator/sdcard/resapp-compact.apk")).unwrap();
    assert_eq!(info.label, "Resource Label App");
    assert_eq!(info.icon.as_ref().map(Vec::len), Some(1672));
    assert_eq!(info.sdk(), "min 34 / target 35");
}

#[test]
fn app_list() {
    let out = "package:com.example.zeta\npackage:com.example.Alpha\n---SEP---\npackage:android\npackage:com.example.system\n---SEP---\npackage:com.example.zeta\npackage:com.example.system\n";
    let list = apps::parse_list(out);
    let names: alloc::vec::Vec<_> = list.iter().map(|a| (a.package.as_str(), a.system, a.disabled)).collect();
    assert_eq!(names, [("android", true, false), ("com.example.Alpha", false, false), ("com.example.system", true, true), ("com.example.zeta", false, true)]);
    assert!(apps::parse_list("---SEP---\n---SEP---\n").is_empty());
}

#[test]
fn app_details() {
    let out = "Packages:\n  Package [com.example.app] (1a2b3c):\n    versionCode=42 minSdk=28 targetSdk=35\n    versionName=1.2.3\n    timeStamp=2026-01-02 03:04:05\n    firstInstallTime=2025-12-31 23:59:58\n    lastUpdateTime=2026-01-02 03:04:05\n\nHidden system packages:\n  Package [com.example.app] (4d5e6f):\n    versionName=1.0\n---SEP---\npackage:/data/app/~~AAAA==/com.example.app-BBBB==/base.apk\npackage:/data/app/~~AAAA==/com.example.app-BBBB==/split_config.en.apk\n";
    let details = apps::parse_details(out, "com.example.app");
    assert_eq!(details.version, "1.2.3");
    assert_eq!(details.installed, "2025-12-31 23:59");
    assert_eq!(details.updated, "2026-01-02 03:04");
    assert_eq!(details.path, "/data/app/~~AAAA==/com.example.app-BBBB==/base.apk");
    assert_eq!(apps::parse_details("---SEP---\n", "com.example.app"), apps::Details::default());
}

#[test]
fn app_actions() {
    assert_eq!(Action::Disable.command("com.example.app"), "pm disable-user --user 0 'com.example.app' 2>&1");
    assert!(Action::Launch.succeeded("  bash arg: -p\nEvents injected: 1\n## Network stats: elapsed time=10ms\n"));
    assert!(!Action::Launch.succeeded("  bash arg: -p\n** No activities found to run, monkey aborted.\n"));
    assert!(Action::ForceStop.succeeded("\n"));
    assert!(Action::Uninstall.succeeded("Success\n"));
    assert!(!Action::ClearData.succeeded("Failed\n"));
    assert!(Action::Disable.succeeded("Package com.example.app new state: disabled-user\n"));
    assert!(Action::Enable.succeeded("Package com.example.app new state: enabled\n"));
    assert_eq!(apps::failure_reason("  bash arg: -p\n  bash arg: x\n** No activities found to run, monkey aborted.\n"), "** No activities found to run, monkey aborted.");
    assert_eq!(apps::failure_reason("Failure [DELETE_FAILED_INTERNAL_ERROR]\n"), "Failure [DELETE_FAILED_INTERNAL_ERROR]");
    assert_eq!(apps::failure_reason(""), "The command failed.");
}

#[test]
fn device_date() {
    assert_eq!(clock::parse_date("1800000000 +0900\n"), Some((1_800_000_000, 540)));
    assert_eq!(clock::parse_date("1800000000 -0330"), Some((1_800_000_000, -210)));
    assert_eq!(clock::parse_date("1800000000 +0000"), Some((1_800_000_000, 0)));
    assert_eq!(clock::parse_date("1800000000 0900"), None);
    assert_eq!(clock::parse_date("1800000000 +09"), None);
    assert_eq!(clock::parse_date("1800000000 +0960"), None);
    assert_eq!(clock::parse_date("date: bad format"), None);
    assert_eq!(clock::parse_date(""), None);
}

const LOG_D: &str = "10-25 05:29:49.510  2030  2091 D WindowManager: Remove SleepToken: tag=keyguard";
const LOG_W: &str = "10-25 05:29:49.574  5349  5349 W df      : type=1400 audit(0.0:8506): avc: denied";
const LOG_E: &str = "10-25 05:29:50.159  4796  4796 E ConsumerBase: abandonImpl";

fn shown(log: &Log) -> Vec<&str> {
    (0..log.shown_len()).map(|i| log.get(log.shown_seq(i).unwrap()).unwrap().text).collect()
}

#[test]
fn logcat_line_fields() {
    let mut log = Log::new();
    log.push(LOG_D);
    log.push(LOG_W);
    log.push("--------- beginning of main");
    let d = log.get(0).unwrap();
    let h = d.header().unwrap();
    assert_eq!((h.time, h.pid, h.tid, h.tag), ("10-25 05:29:49.510", 2030, 2091, "WindowManager"));
    assert_eq!((d.level, d.message()), (Level::Debug, "Remove SleepToken: tag=keyguard"));
    let w = log.get(1).unwrap();
    assert_eq!(w.header().unwrap().tag, "df");
    assert_eq!(w.message(), "type=1400 audit(0.0:8506): avc: denied");
    let marker = log.get(2).unwrap();
    assert!(marker.header().is_none());
    assert_eq!((marker.level, marker.message()), (Level::Warning, "--------- beginning of main"));
}

#[test]
fn logcat_filters() {
    assert_eq!(Filter::parse("  "), Filter::All);
    assert_eq!(Filter::parse("TAG: windowmanager"), Filter::Tag("windowmanager".into()));
    assert_eq!(Filter::parse("pid:5349"), Filter::Pid(5349));
    assert_eq!(Filter::parse("pid:x"), Filter::Text("pid:x".into()));
    let mut log = Log::new();
    for line in [LOG_D, LOG_W, LOG_E] {
        log.push(line);
    }
    log.set_min_level(Level::Warning);
    assert_eq!(shown(&log), [LOG_W, LOG_E]);
    log.set_min_level(Level::Verbose);
    log.set_filter(Filter::parse("tag:windowmanager"));
    assert_eq!(shown(&log), [LOG_D]);
    log.set_filter(Filter::parse("tag:Window"));
    assert!(shown(&log).is_empty());
    log.set_filter(Filter::parse("pid:4796"));
    assert_eq!(shown(&log), [LOG_E]);
    log.set_filter(Filter::parse("AVC: DENIED"));
    assert_eq!(shown(&log), [LOG_W]);
    log.push(LOG_D);
    log.push("10-25 05:29:51.000   100   100 I x: avc: denied again");
    assert_eq!(shown(&log).len(), 2);
    assert_eq!(log.shown_index(4), Some(1));
}

#[test]
fn logcat_resume_skips_what_came_before() {
    let mut log = Log::new();
    log.push(LOG_D);
    log.push(LOG_W);
    log.resume();
    log.push("--------- beginning of main");
    log.push(LOG_D);
    log.push(LOG_W);
    log.push("10-25 05:29:49.574  5349  5349 W df      : same time, new text");
    log.push(LOG_E);
    assert_eq!(log.shown_len(), 4);
    assert_eq!(log.last_time(), Some("10-25 05:29:50.159"));
}

#[test]
fn logcat_evicts_the_oldest_chunk() {
    let mut log = Log::new();
    let line = alloc::format!("10-25 05:29:49.510  2030  2091 I Tag: {}", "x".repeat(900));
    let mut removed = 0;
    for _ in 0..3000 {
        removed += log.push(&line);
    }
    assert!(removed > 0);
    assert_eq!(log.shown_len() + removed, 3000);
    let first = log.shown_seq(0).unwrap();
    assert_eq!(first as usize, removed);
    assert!(log.get(first - 1).is_none());
    assert_eq!(log.get(2999).unwrap().text, line);
    log.clear();
    assert_eq!(log.shown_len(), 0);
    log.push(LOG_E);
    assert_eq!(log.shown_seq(0), Some(3000));
}

#[test]
fn logcat_lines_split_across_reads() {
    let mut splitter = LineSplitter::new();
    let mut lines = Vec::new();
    splitter.feed(b"first\nsec", |l| lines.push(l.to_string()));
    splitter.feed(b"ond\r\n\nthird", |l| lines.push(l.to_string()));
    assert_eq!(lines, ["first", "second\r", ""]);
}
