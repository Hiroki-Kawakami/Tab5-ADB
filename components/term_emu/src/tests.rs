/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::string::String;

use crate::{Cell, Key, Terminal, attr};

fn text(t: &Terminal, row: usize, offset: usize) -> String {
    let line: String = t.row(row, offset).iter().map(|c| c.ch as char).collect();
    String::from(line.trim_end())
}

fn small() -> Terminal {
    Terminal::new(10, 5, 20)
}

#[test]
fn print_and_controls() {
    let mut t = small();
    t.feed(b"hello");
    assert_eq!(text(&t, 0, 0), "hello");
    assert_eq!(t.cursor(), (0, 5));
    t.feed(b"\r\nworld");
    assert_eq!(text(&t, 1, 0), "world");
    t.feed(b"\rX");
    assert_eq!(text(&t, 1, 0), "Xorld");
    t.feed(b"\x08\x08Y");
    assert_eq!(text(&t, 1, 0), "Yorld");
    t.feed(b"\rZ\tT");
    assert_eq!(text(&t, 1, 0), "Zorld   T");
}

#[test]
fn deferred_wrap() {
    let mut t = small();
    t.feed(b"0123456789");
    assert_eq!(t.cursor(), (0, 9));
    t.feed(b"a");
    assert_eq!(t.cursor(), (1, 1));
    assert_eq!(text(&t, 0, 0), "0123456789");
    assert_eq!(text(&t, 1, 0), "a");
    t.feed(b"\x1b[1;1H0123456789\rB");
    assert_eq!(text(&t, 0, 0), "B123456789");
    assert_eq!(t.cursor().0, 0);

    let mut u = small();
    u.feed(b"\x1b[?7l0123456789XY");
    assert_eq!(text(&u, 0, 0), "012345678Y");
    assert_eq!(u.cursor().0, 0);
}

#[test]
fn cursor_moves() {
    let mut t = small();
    t.feed(b"\x1b[3;4H");
    assert_eq!(t.cursor(), (2, 3));
    t.feed(b"\x1b[A\x1b[2C");
    assert_eq!(t.cursor(), (1, 5));
    t.feed(b"\x1b[10D");
    assert_eq!(t.cursor().1, 0);
    t.feed(b"\x1b[7G");
    assert_eq!(t.cursor().1, 6);
    t.feed(b"\x1b[2d");
    assert_eq!(t.cursor(), (1, 6));
    t.feed(b"\x1b[99;99H");
    assert_eq!(t.cursor(), (4, 9));
    t.feed(b"\x1b[H");
    assert_eq!(t.cursor(), (0, 0));
}

#[test]
fn erase() {
    let mut t = small();
    t.feed(b"aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc");
    t.feed(b"\x1b[2;5H\x1b[K");
    assert_eq!(text(&t, 1, 0), "bbbb");
    t.feed(b"\x1b[1K");
    assert_eq!(text(&t, 1, 0), "");
    t.feed(b"\x1b[1;1H\x1b[J");
    assert_eq!(text(&t, 0, 0), "");
    assert_eq!(text(&t, 2, 0), "");
}

#[test]
fn insert_delete() {
    let mut t = small();
    t.feed(b"abcdefghij");
    t.feed(b"\x1b[1;3H\x1b[2@");
    assert_eq!(text(&t, 0, 0), "ab  cdefgh");
    t.feed(b"\x1b[2P");
    assert_eq!(text(&t, 0, 0), "abcdefgh");
    t.feed(b"\x1b[3X");
    assert_eq!(text(&t, 0, 0), "ab   fgh");

    let mut u = small();
    u.feed(b"one\r\ntwo\r\nthree");
    u.feed(b"\x1b[2;1H\x1b[L");
    assert_eq!(text(&u, 1, 0), "");
    assert_eq!(text(&u, 2, 0), "two");
    assert_eq!(text(&u, 3, 0), "three");
    u.feed(b"\x1b[M");
    assert_eq!(text(&u, 1, 0), "two");
    assert_eq!(text(&u, 2, 0), "three");
}

#[test]
fn insert_mode() {
    let mut t = small();
    t.feed(b"abc\r\x1b[4hX\x1b[4lY");
    assert_eq!(text(&t, 0, 0), "XYbc");
}

#[test]
fn scroll_region() {
    let mut t = small();
    t.feed(b"r1\r\nr2\r\nr3\r\nr4\r\nr5");
    t.feed(b"\x1b[2;4r");
    assert_eq!(t.cursor(), (0, 0));
    t.feed(b"\x1b[4;1H\n");
    assert_eq!(text(&t, 0, 0), "r1");
    assert_eq!(text(&t, 1, 0), "r3");
    assert_eq!(text(&t, 2, 0), "r4");
    assert_eq!(text(&t, 3, 0), "");
    assert_eq!(text(&t, 4, 0), "r5");
    t.feed(b"\x1b[2;1H\x1bM");
    assert_eq!(text(&t, 1, 0), "");
    assert_eq!(text(&t, 2, 0), "r3");
    t.feed(b"\x1b[r");
    assert_eq!(t.history_len(), 0);
}

#[test]
fn history() {
    let mut t = Terminal::new(10, 3, 5);
    t.feed(b"L1\r\nL2\r\nL3\r\nL4\r\nL5");
    assert_eq!(text(&t, 0, 0), "L3");
    assert_eq!(t.history_len(), 2);
    assert_eq!(text(&t, 0, 1), "L2");
    assert_eq!(text(&t, 0, 2), "L1");
    assert_eq!(text(&t, 2, 2), "L3");
    t.feed(b"\r\nL6\r\nL7\r\nL8\r\nL9");
    assert_eq!(t.history_len(), 5);
    assert_eq!(text(&t, 0, 5), "L2");
    t.feed(b"\x1b[3J");
    assert_eq!(t.history_len(), 0);
}

#[test]
fn sgr() {
    let mut t = small();
    t.feed(b"\x1b[31mr\x1b[1;32mg\x1b[0;48;5;200mp\x1b[4;7mu\x1b[mz");
    let r = t.row(0, 0);
    assert!(r[0].fg == 1 && r[0].attr & attr::FG_DEFAULT == 0);
    assert!(r[1].fg == 2 && r[1].attr & attr::BOLD != 0);
    assert!(r[2].bg == 200 && r[2].attr & attr::BG_DEFAULT == 0);
    assert!(r[2].attr & attr::FG_DEFAULT != 0);
    assert!(r[2].attr & attr::BOLD == 0);
    assert!(r[3].attr & attr::UNDERLINE != 0 && r[3].attr & attr::REVERSE != 0);
    assert!(r[4].attr & attr::FG_DEFAULT != 0 && r[4].attr & attr::BG_DEFAULT != 0);
    t.feed(b"\r\n\x1b[95mB\x1b[38;2;255;0;0mT");
    let r = t.row(1, 0);
    assert_eq!(r[0].fg, 13);
    assert_eq!(r[1].fg, 196);
    t.feed(b"\x1b[44m\x1b[2;5H\x1b[K");
    let r = t.row(1, 0);
    assert!(r[6].bg == 4 && r[6].attr & attr::BG_DEFAULT == 0);
    assert_eq!(r[6].ch, b' ');
}

#[test]
fn modes_and_responses() {
    let mut t = small();
    assert_eq!(t.key(Key::Up), b"\x1b[A");
    t.feed(b"\x1b[?1h");
    assert_eq!(t.key(Key::Up), b"\x1bOA");
    assert_eq!(t.key(Key::Home), b"\x1bOH");
    t.feed(b"\x1b[?1l");
    assert_eq!(t.key(Key::Up), b"\x1b[A");
    assert!(t.cursor_visible());
    t.feed(b"\x1b[?25l");
    assert!(!t.cursor_visible());
    t.feed(b"\x1b[?25h\x1b[2;3H\x1b[6n");
    assert_eq!(t.take_response(), b"\x1b[2;3R");
    t.feed(b"\x1b[c");
    assert_eq!(t.take_response(), b"\x1b[?6c");
    assert!(t.take_response().is_empty());
}

#[test]
fn alt_screen() {
    let mut t = small();
    t.feed(b"main\x1b[?1049h");
    assert!(t.alt_screen());
    assert_eq!(text(&t, 0, 0), "");
    t.feed(b"\x1b[HALT");
    assert_eq!(text(&t, 0, 0), "ALT");
    t.feed(b"\x1b[?1049l");
    assert!(!t.alt_screen());
    assert_eq!(text(&t, 0, 0), "main");
    assert_eq!(t.cursor().1, 4);
}

#[test]
fn osc_and_charset() {
    let mut t = small();
    t.feed(b"\x1b]0;window title\x07ok");
    assert_eq!(text(&t, 0, 0), "ok");
    t.feed(b"\r\x1b]2;t\x1b\\ko");
    assert_eq!(text(&t, 0, 0), "ko");
    t.feed(b"\r\n\x1b(0lqqk\x1b(Bza");
    assert_eq!(text(&t, 1, 0), "+--+za");
}

#[test]
fn utf8() {
    let mut t = small();
    t.feed("a\u{e9}b".as_bytes());
    assert_eq!(text(&t, 0, 0), "a?b");
    assert_eq!(t.cursor().1, 3);
    t.feed("\r\n\u{65e5}x".as_bytes());
    assert_eq!(text(&t, 1, 0), "? x");
    assert_eq!(t.cursor().1, 3);
    t.feed(b"\r\n\xff y");
    assert_eq!(text(&t, 2, 0), "? y");
}

#[test]
fn rep_and_reset() {
    let mut t = small();
    t.feed(b"=\x1b[4b");
    assert_eq!(text(&t, 0, 0), "=====");
    t.feed(b"\x1bc");
    assert_eq!(text(&t, 0, 0), "");
    assert_eq!(t.cursor(), (0, 0));
    assert_eq!(t.history_len(), 0);
}

#[test]
fn dirty_rows() {
    let mut t = small();
    assert_eq!(t.take_dirty(), Some(0..5));
    assert_eq!(t.take_dirty(), None);
    t.feed(b"\x1b[3;1Hx\x1b[2;1Hy");
    assert_eq!(t.take_dirty(), Some(1..3));
}

#[test]
fn resize_keeps_the_cursor_line() {
    let mut t = Terminal::new(10, 5, 20);
    t.feed(b"L1\r\nL2\r\nL3\r\nL4\r\n$ ");
    t.resize(10, 3);
    assert_eq!((t.rows(), t.cols()), (3, 10));
    assert_eq!(text(&t, 0, 0), "L3");
    assert_eq!(text(&t, 2, 0), "$");
    assert_eq!(t.cursor(), (2, 2));
    assert_eq!(t.history_len(), 2);
    assert_eq!(text(&t, 0, 2), "L1");

    t.resize(10, 5);
    assert_eq!(t.history_len(), 0);
    assert_eq!(text(&t, 0, 0), "L1");
    assert_eq!(text(&t, 4, 0), "$");
    assert_eq!(t.cursor(), (4, 2));
}

#[test]
fn resize_columns() {
    let mut t = Terminal::new(10, 2, 20);
    t.feed(b"0123456789\r\nab\r\ncd");
    t.resize(4, 2);
    assert_eq!(text(&t, 0, 1), "0123");
    assert_eq!(text(&t, 0, 0), "ab");
    assert_eq!(t.cursor(), (1, 2));
    t.resize(12, 2);
    assert_eq!(text(&t, 0, 1), "0123");
    t.feed(b"0123456789");
    assert_eq!(text(&t, 1, 0), "cd0123456789");
}

#[test]
fn resize_on_the_alternate_screen() {
    let mut t = Terminal::new(10, 5, 20);
    t.feed(b"a\r\nb\r\nc\r\nd\r\ne\x1b[?1049h\x1b[5;1Hz");
    t.resize(10, 3);
    assert!(t.alt_screen());
    assert_eq!(t.cursor().0, 2);
    t.feed(b"\x1b[?1049l");
    assert_eq!(text(&t, 0, 0), "c");
    assert_eq!(text(&t, 2, 0), "e");
    assert_eq!(t.cursor(), (2, 1));
}

#[test]
fn split_fuzz() {
    let stream: &[u8] = b"plain text\r\n\
        \x1b[2;10r\x1b[31;1mred bold\x1b[0m\r\n\
        \x1b]0;ti\x1b tle\x07\
        \x1b[3;5H\x1b[2J\x1b[38;5;123mX\x1b[48;2;1;2;3mY\r\n\
        tab\there\r\n\
        \xc3\xa9\xe6\x97\xa5 wide\r\n\
        \x1b(0qqx\x1b(B\r\n\
        \x1b[?1049hALT\x1b[5;1Hbottom\x1b[?1049l\
        \x1b[2A\x1b[3C\x1b[Kend\x1b[?25l\x1b[?7l\
        0123456789012345678901234567890123456789012345678901234567890123456789012\r\n\
        \x1b[?7h\x1b[4h ins\x1b[4l\x1b[10;1Hscroll\nscroll\nscroll\n";

    let mut whole = Terminal::new(72, 12, 50);
    whole.feed(stream);

    let mut lcg: u32 = 12345;
    for trial in 0..50 {
        let mut chunked = Terminal::new(72, 12, 50);
        let mut pos = 0;
        while pos < stream.len() {
            lcg = lcg.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            let n = (1 + (lcg >> 24) as usize % 7).min(stream.len() - pos);
            chunked.feed(&stream[pos..pos + n]);
            pos += n;
        }
        assert_eq!(chunked.cursor(), whole.cursor(), "trial {trial}");
        assert_eq!(chunked.history_len(), whole.history_len(), "trial {trial}");
        assert_eq!(chunked.alt_screen(), whole.alt_screen(), "trial {trial}");
        assert_eq!(chunked.cursor_visible(), whole.cursor_visible(), "trial {trial}");
        for row in 0..whole.rows() {
            let same: bool = whole.row(row, 0).iter().zip(chunked.row(row, 0)).all(|(a, b): (&Cell, &Cell)| a == b);
            assert!(same, "trial {trial} row {row}");
        }
    }
}
