/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::vec::Vec;

use crate::protocol::{Channel, ENOTSUP, FrameHead, HELLO, Header, Hello, HelloReply, REPLY, TextImage, Touch, TouchPoint, VideoStart, adapt_display, app_entries, get_app_info, key, message, reply};

#[test]
fn header_round_trip() {
    let mut out = Vec::new();
    message(&mut out, REPLY, 0x1234, b"abc");
    assert_eq!(out, [3, 0, 0, 0, 0x80, 0, 0x34, 0x12, b'a', b'b', b'c']);
    let head: [u8; 8] = out[..8].try_into().unwrap();
    assert_eq!(Header::parse(&head), Header { length: 3, kind: REPLY, id: 0x1234 });
}

#[test]
fn hello_bytes() {
    let hello = Hello { proto_version: 1, capabilities: 0x0102_0304, max_message: [0x10, 0x20, 0x30] };
    let mut out = Vec::new();
    message(&mut out, HELLO, 1, &hello.encode());
    assert_eq!(
        out,
        [
            20, 0, 0, 0, 0x01, 0, 1, 0, //
            1, 0, 0, 0, 4, 3, 2, 1, //
            0x10, 0, 0, 0, 0x20, 0, 0, 0, 0x30, 0, 0, 0,
        ]
    );
}

#[test]
fn hello_reply() {
    let mut payload = alloc::vec![0, 0, 0, 0, 1, 0, 2, 3, 4, 0, 0, 0, 8, 0, 0, 0, 0, 0, 1, 0, 0xee];
    let (status, result) = reply(&payload).unwrap();
    assert_eq!(status, 0);
    assert_eq!(
        HelloReply::parse(result),
        Some(HelloReply { proto_version: 1, version: [2, 3, 4], capabilities: 8, max_message: 0x10000 })
    );
    payload.truncate(19);
    assert_eq!(HelloReply::parse(reply(&payload).unwrap().1), None);
}

#[test]
fn reply_status() {
    assert_eq!(reply(&[ENOTSUP, 0, 0, 0]), Some((ENOTSUP, &[][..])));
    assert_eq!(reply(&[0, 0, 0]), None);
}

#[test]
fn services() {
    assert_eq!(Channel::Control.service("0A1B2C3D"), "localabstract:tab5adb-agent-0A1B2C3D");
    assert_eq!(Channel::Video.service("0A1B2C3D"), "localabstract:tab5adb-agent-0A1B2C3D-video");
    assert_eq!(Channel::Audio.service("0A1B2C3D"), "localabstract:tab5adb-agent-0A1B2C3D-audio");
}

#[test]
fn video_start_bytes() {
    let start = VideoStart { box_width: 720, box_height: 1280, swap: true, quality: 60, max_fps: 30, strips: 4, fill: false };
    assert_eq!(start.encode(), [0xd0, 0x02, 0x00, 0x05, 1, 60, 30, 4]);
    assert_eq!(VideoStart { swap: false, fill: true, ..start }.encode()[4], 2);
}

#[test]
fn frame_head() {
    let head = FrameHead::parse(&[0x00, 0x05, 0xd0, 0x02, 0x30, 0x01, 0xa0, 0x00, 0x03, 0x01, 0, 0]);
    assert_eq!(head, FrameHead { width: 1280, height: 720, y: 304, rows: 160, rotation: 3, last: true });
}

#[test]
fn touch_bytes() {
    let touch = Touch { time_ms: 0x0102_0304, width: 720, height: 1280, rotation: 1, points: alloc::vec![TouchPoint { id: 7, x: 0x123, y: 0x456 }] };
    assert_eq!(touch.encode(), [4, 3, 2, 1, 0xd0, 0x02, 0x00, 0x05, 1, 1, 0, 0, 7, 0, 0x23, 0x01, 0x56, 0x04]);
    assert_eq!(Touch { points: Vec::new(), ..touch }.encode().len(), 12);
}

#[test]
fn key_bytes() {
    assert_eq!(key(187, true), [187, 0, 0, 0]);
    assert_eq!(key(0x0123, false), [0x23, 0x01, 1, 0]);
}

#[test]
fn adapt_display_bytes() {
    assert_eq!(adapt_display(720, 1280), [0xd0, 0x02, 0x00, 0x05]);
    assert_eq!(adapt_display(0, 0), [0; 4]);
}

#[test]
fn get_app_info_bytes() {
    assert_eq!(get_app_info(64, 28, &["a.b", "c"]), Some(alloc::vec![64, 0, 28, 2, 3, b'a', b'.', b'b', 1, b'c']));
    assert_eq!(get_app_info(64, 28, &[]), None);
}

#[test]
fn text_image() {
    let b = [3, 0, 2, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 2, 3, 4, 5, 6, 0xee];
    let (image, rest) = TextImage::parse(&b).unwrap();
    assert_eq!((image.width, image.height, image.ascent), (3, 2, 1));
    assert_eq!(image.breaks().collect::<Vec<_>>(), [1, 3]);
    assert_eq!(image.alpha, [1, 2, 3, 4, 5, 6]);
    assert_eq!(rest, [0xee]);
    assert_eq!(TextImage::parse(&b[..17]), None);
    let empty = [0, 0, 20, 0, 16, 0, 0, 0];
    assert_eq!(TextImage::parse(&empty).unwrap().0.alpha, &[] as &[u8]);
}

#[test]
fn app_entries_bytes() {
    let mut b = alloc::vec![0, 0, 0, 0];
    b.extend_from_slice(&[1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0x7f, 1, 0, 1, 0, 9, 8, 7, 6]);
    b.extend_from_slice(&[1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0]);
    let entries = app_entries(&b, 3).unwrap();
    assert_eq!(entries[0], None);
    let first = entries[1].unwrap();
    assert_eq!((first.name.alpha, first.icon_width, first.icon_height, first.icon), (&[0x7f][..], 1, 1, &[9, 8, 7, 6][..]));
    let second = entries[2].unwrap();
    assert_eq!((second.name.width, second.icon), (0, &[][..]));
    assert_eq!(app_entries(&b[..b.len() - 1], 3), None);
}
