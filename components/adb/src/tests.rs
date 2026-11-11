/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use std::sync::Arc;
use std::time::Duration;

use idf::executor::Spawner;

use crate::Result;
use crate::link::base64;
use crate::link::connection::banner_has_feature;
use crate::link::identity::{n0inv, r_squared};
use crate::link::packet::{A_CNXN, A_OKAY, A_OPEN, A_VERSION, A_WRTE, Header, Packet};
use crate::link::transport::{BoxFuture, PacketReader, PacketWriter, Transport};
use crate::link::{Connection, Identity};
use crate::link::pair::Cipher;
use crate::link::spake2::{Role, Spake2};
use crate::services::shell::{packet, window_size};
use crate::services::sync::stat_v2;

#[test]
fn packet_header() {
    let p = Packet::new(A_CNXN, A_VERSION, 4096, b"host::\0".to_vec());
    let h = p.header();
    assert_eq!(&h[..4], b"CNXN");
    assert_eq!(&h[12..16], &7u32.to_le_bytes());
    let sum: u32 = b"host::\0".iter().map(|&b| u32::from(b)).sum();
    assert_eq!(&h[16..20], &sum.to_le_bytes());
    assert_eq!(Header::parse(&h), Some(Header { command: A_CNXN, arg0: A_VERSION, arg1: 4096, length: 7 }));
    let mut bad = h;
    bad[20] ^= 1;
    assert_eq!(Header::parse(&bad), None);
}

#[test]
fn base64_vectors() {
    for (input, output) in [("", ""), ("f", "Zg=="), ("fo", "Zm8="), ("foo", "Zm9v"), ("foobar", "Zm9vYmFy")] {
        assert_eq!(base64::encode(input.as_bytes()), output.as_bytes());
    }
}

#[test]
fn r_squared_matches_u128() {
    for n in [0xffff_ffff_ffff_ffc5u64, 0x8000_0000_0000_001d, 0x0123_4567_89ab_cdef] {
        let r = (1u128 << 64) % u128::from(n);
        let expected = (r * r) % u128::from(n);
        let words = r_squared(&[n as u32, (n >> 32) as u32]);
        assert_eq!(u128::from(words[0]) | u128::from(words[1]) << 32, expected);
    }
}

#[test]
fn n0inv_inverts() {
    for n0 in [1u32, 3, 0x1234_5679, 0xffff_ffff] {
        assert_eq!(n0.wrapping_mul(n0inv(n0)), u32::MAX);
    }
}

#[test]
fn shell_protocol_packets() {
    assert_eq!(packet(0, b"ls\r"), [0, 3, 0, 0, 0, b'l', b's', b'\r']);
    assert_eq!(window_size(42, 72), b"42x72,0x0\0");
}

#[test]
fn banner_features() {
    let banner = "device::ro.product.model=Phone;features=shell_v2,cmd,stat_v2,ls_v2";
    assert!(banner_has_feature(banner, "stat_v2"));
    assert!(!banner_has_feature(banner, "stat"));
    assert!(!banner_has_feature("device::ro.product.model=Phone", "stat_v2"));
}

#[test]
fn stat_v2_reply() {
    let mut reply = [0u8; 72];
    reply[..4].copy_from_slice(b"STA2");
    reply[24..28].copy_from_slice(&0o040755u32.to_le_bytes());
    reply[40..48].copy_from_slice(&0x1_0000_0002u64.to_le_bytes());
    reply[56..64].copy_from_slice(&1_700_000_000i64.to_le_bytes());
    let stat = stat_v2(&reply).unwrap();
    assert!(stat.is_dir());
    assert_eq!((stat.size, stat.mtime), (0x1_0000_0002, 1_700_000_000));
    reply[4..8].copy_from_slice(&2u32.to_le_bytes());
    assert!(!stat_v2(&reply).unwrap().exists());
    reply[..4].copy_from_slice(b"STAT");
    assert!(stat_v2(&reply).is_err());
}

fn hex(data: &[u8]) -> String {
    data.iter().map(|b| format!("{b:02x}")).collect()
}

fn unhex(text: &str) -> Vec<u8> {
    (0..text.len()).step_by(2).map(|i| u8::from_str_radix(&text[i..i + 2], 16).unwrap()).collect()
}

fn oracle(args: &[&str], stdin: bool) -> std::process::Child {
    let path = std::env::var("ADB_SPAKE2_ORACLE").expect("run test/run.sh, which builds the BoringSSL oracle");
    let mut command = std::process::Command::new(path);
    command.args(args).stdout(std::process::Stdio::piped());
    if stdin {
        command.stdin(std::process::Stdio::piped());
    }
    command.spawn().unwrap()
}

/// Our side as `role` against BoringSSL as the other; both keys, or `None`
/// where a side failed.
fn spake2_against_boringssl(role: Role, ours: &[u8], theirs: &[u8], random: &[u8; 64]) -> (Option<[u8; 64]>, Option<Vec<u8>>) {
    use std::io::{BufRead, BufReader, Write};
    let peer = if role == Role::Alice { "bob" } else { "alice" };
    let mut child = oracle(&["spake2", peer, &hex(theirs)], true);
    let mut stdout = BufReader::new(child.stdout.take().unwrap());
    let mut line = String::new();
    stdout.read_line(&mut line).unwrap();
    let their_msg: [u8; 32] = unhex(line.trim()).try_into().unwrap();
    let spake2 = Spake2::new(role, ours, random);
    writeln!(child.stdin.take().unwrap(), "{}", hex(spake2.message())).unwrap();
    line.clear();
    stdout.read_line(&mut line).unwrap();
    assert!(child.wait().unwrap().success());
    let line = line.trim();
    (spake2.finish(&their_msg), (line != "FAIL").then(|| unhex(line)))
}

fn password(code: &[u8]) -> Vec<u8> {
    let mut password = code.to_vec();
    password.extend((0..64).map(|i| (i * 37 + 11) as u8));
    password
}

#[test]
fn spake2_matches_boringssl() {
    for (i, role) in [Role::Alice, Role::Bob].into_iter().enumerate() {
        for round in 0..4u8 {
            let random: [u8; 64] = core::array::from_fn(|j| (j as u8).wrapping_mul(29).wrapping_add(round * 7 + i as u8));
            let password = password(b"123456");
            let (ours, theirs) = spake2_against_boringssl(role, &password, &password, &random);
            assert_eq!(ours.map(|k| k.to_vec()), theirs, "{role:?} round {round}");
        }
    }
}

#[test]
fn spake2_wrong_code_differs() {
    let (ours, theirs) = spake2_against_boringssl(Role::Alice, &password(b"123456"), &password(b"123457"), &[3; 64]);
    assert_ne!(ours.map(|k| k.to_vec()), theirs);
}

#[test]
fn pairing_records_match_boringssl() {
    let material: Vec<u8> = (0..64).map(|i| (i * 13 + 5) as u8).collect();
    let plain: Vec<u8> = (0..8192).map(|i| (i % 251) as u8).collect();
    let output = |args: &[&str]| {
        let out = oracle(args, false).wait_with_output().unwrap();
        String::from_utf8(out.stdout).unwrap().trim().to_string()
    };
    let mut sealing = Cipher::new(&material).unwrap();
    let mut opening = Cipher::new(&material).unwrap();
    for sequence in 0..2 {
        let mut record = plain.clone();
        record.resize(plain.len() + 16, 0);
        sealing.seal(&mut record).unwrap();
        assert_eq!(hex(&record), output(&["seal", &hex(&material), &sequence.to_string(), &hex(&plain)]));
        assert_eq!(hex(&plain), output(&["open", &hex(&material), &sequence.to_string(), &hex(&record)]));
        assert_eq!(opening.open(&mut record).unwrap(), &plain[..]);
    }
    let mut record = plain.clone();
    record.resize(plain.len() + 16, 0);
    Cipher::new(&material).unwrap().seal(&mut record).unwrap();
    record[0] ^= 1;
    assert!(Cipher::new(&material).unwrap().open(&mut record).is_err());
}

struct FakeDevice {
    from_host: idf::executor::Receiver<Packet>,
    to_host: idf::executor::Sender<Packet>,
}

struct FakeTransport {
    from_device: idf::executor::Receiver<Packet>,
    to_device: idf::executor::Sender<Packet>,
}

struct FakeReader(idf::executor::Receiver<Packet>);
struct FakeWriter(idf::executor::Sender<Packet>);

impl Transport for FakeTransport {
    fn split(self: Box<Self>) -> (Box<dyn PacketReader>, Box<dyn PacketWriter>) {
        (Box::new(FakeReader(self.from_device)), Box::new(FakeWriter(self.to_device)))
    }
}

impl PacketReader for FakeReader {
    fn read(&mut self) -> BoxFuture<'_, Result<Packet>> {
        Box::pin(async { Ok(self.0.recv().await) })
    }
}

impl PacketWriter for FakeWriter {
    fn write<'a>(&'a mut self, packet: &'a Packet) -> BoxFuture<'a, Result<()>> {
        let copy = Packet::new(packet.command, packet.arg0, packet.arg1, packet.payload.segments().flatten().copied().collect());
        Box::pin(async move {
            self.0.send(copy).await;
            Ok(())
        })
    }
}

impl FakeDevice {
    fn recv(&mut self) -> Packet {
        idf::executor::block_on(idf::executor::timeout(Duration::from_secs(5), self.from_host.recv())).expect("no packet from the host")
    }

    fn idle(&mut self) -> bool {
        idf::executor::block_on(idf::executor::timeout(Duration::from_millis(100), self.from_host.recv())).is_err()
    }

    fn send(&self, command: u32, arg0: u32, arg1: u32, payload: &[u8]) {
        assert!(self.to_host.try_send(Packet::new(command, arg0, arg1, payload.to_vec())).is_ok());
    }
}

fn wait<T>(handle: idf::executor::JoinHandle<T>) -> T {
    idf::executor::block_on(idf::executor::timeout(Duration::from_secs(5), handle)).expect("the host did not finish")
}

fn online(features: &str) -> (Connection, FakeDevice, Spawner) {
    let (to_device, from_host) = idf::executor::channel(64);
    let (to_host, from_device) = idf::executor::channel(64);
    let spawner = idf::thread::Builder::new().name("adb_test").stack_size(32 * 1024).spawn_local_executor(|_| {}).unwrap();
    static IDENTITY: std::sync::OnceLock<Arc<Identity>> = std::sync::OnceLock::new();
    let identity = IDENTITY.get_or_init(|| Arc::new(Identity::generate("test").unwrap())).clone();
    let conn = Connection::connect(Box::new(FakeTransport { from_device, to_device }), identity, &spawner);
    let mut device = FakeDevice { from_host, to_host };
    let cnxn = device.recv();
    assert_eq!(cnxn.command, A_CNXN);
    assert!(banner_has_feature(core::str::from_utf8(&cnxn.payload.into_vec()).unwrap(), "delayed_ack"));
    device.send(A_CNXN, A_VERSION, 4096, format!("device::features={features}").as_bytes());
    let waiting = conn.clone();
    wait(spawner.spawn(async move { waiting.online().await.unwrap() }));
    (conn, device, spawner)
}

fn acked(packet: &Packet) -> u32 {
    let payload = packet.payload.segments().flatten().copied().collect::<Vec<u8>>();
    u32::from_le_bytes(payload.try_into().expect("OKAY without 4 bytes"))
}

#[test]
fn delayed_ack_windows() {
    let (conn, mut device, spawner) = online("shell_v2,delayed_ack");
    let opener = conn.clone();
    let opening = spawner.spawn(async move { opener.open_with_window("sync:", 1000).await });
    let open = device.recv();
    assert_eq!((open.command, open.arg1), (A_OPEN, 1000));
    let local = open.arg0;
    device.send(A_OKAY, 7, local, &5000u32.to_le_bytes());
    let (mut reader, mut writer) = wait(opening).unwrap().into_split();

    for _ in 0..3 {
        device.send(A_WRTE, 7, local, &[1; 400]);
    }
    let read = wait(spawner.spawn(async move {
        let mut buf = [0u8; 400];
        for _ in 0..2 {
            reader.read_exact(&mut buf).await.unwrap();
        }
        reader
    }));
    let okay = device.recv();
    assert_eq!((okay.command, okay.arg0, okay.arg1, acked(&okay)), (A_OKAY, local, 7, 800));
    let mut reader = read;
    wait(spawner.spawn(async move {
        let mut buf = [0u8; 400];
        reader.read_exact(&mut buf).await.unwrap();
    }));
    assert_eq!(acked(&device.recv()), 400);

    let writing = spawner.spawn(async move { writer.write(&[2; 10000]).await });
    for len in [4096, 4096] {
        let wrte = device.recv();
        assert_eq!((wrte.command, wrte.arg0, wrte.arg1, wrte.payload.len()), (A_WRTE, local, 7, len));
    }
    assert!(device.idle());
    device.send(A_OKAY, 7, local, &8192u32.to_le_bytes());
    assert_eq!(device.recv().payload.len(), 1808);
    wait(writing).unwrap();
}

#[test]
fn stop_and_wait_without_delayed_ack() {
    let (conn, mut device, spawner) = online("shell_v2");
    let opener = conn.clone();
    let opening = spawner.spawn(async move { opener.open("sync:").await });
    let open = device.recv();
    assert_eq!((open.command, open.arg1), (A_OPEN, 0));
    let local = open.arg0;
    device.send(A_OKAY, 7, local, &[]);
    let (mut reader, mut writer) = wait(opening).unwrap().into_split();

    device.send(A_WRTE, 7, local, &[1; 400]);
    wait(spawner.spawn(async move {
        let mut buf = [0u8; 400];
        reader.read_exact(&mut buf).await.unwrap();
    }));
    let okay = device.recv();
    assert_eq!((okay.command, okay.payload.len()), (A_OKAY, 0));

    let writing = spawner.spawn(async move { writer.write(&[2; 5000]).await });
    assert_eq!(device.recv().payload.len(), 4096);
    assert!(device.idle());
    device.send(A_OKAY, 7, local, &[]);
    assert_eq!(device.recv().payload.len(), 904);
    assert!(device.idle());
    device.send(A_OKAY, 7, local, &[]);
    wait(writing).unwrap();
}
