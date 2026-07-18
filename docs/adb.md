# ADB host-side client (`embedded_adb`, `adb`, `agent_link`)

Tab5 plays the **ADB host** (like WebADB / ya-webadb): it drives a
USB- or TCP-connected Android device. Only the host side of the protocol is
implemented, modeled on the upstream ADB sources (a read-only local reference,
not vendored — see `[[local-reference-clones]]` in memory).

Two components, split so the low-level engine can keep thread-model-agnostic
host tests while the app-facing layer is free to spend a FreeRTOS task on the
reader loop:

```
app/                 UI, marshals callbacks onto the LVGL thread
  └── adb            app-facing object API: Client/Shell/Sync/Stream, owns the reader task
        └── embedded_adb   protocol engine (AdbConnection/AdbStream), thread-agnostic
```

**`components/adb/README.md` + `components/adb/docs/<surface>.md` are the
front door for the `adb` layer's API contract** (archetypes, threading,
lifetime rules, per-surface wire detail for `Client`/`Shell`/`Sync`/`Stream`) —
read those before touching that code. This page covers what those docs don't:
the lower `embedded_adb` engine, the transport layer, and the app-level
integration decisions.

## `embedded_adb` layering

One concern per file pair, all portable C++ **except the transport**:

- `adb_protocol` — pure wire format (24-byte `amessage` header, `apacket`,
  command/auth constants, checksum). No I/O.
- `adb_crypto` — RSA-2048 keygen, token signing, the Android public-key blob.
  Uses mbedTLS on both targets (bundled by ESP-IDF, from Nix on the host) — a
  third-party lib, not an ESP API or board concern, so it's used directly with
  no `idf_compat` shim.
- `adb_keystore` — persists the RSA private key via the NVS C API (see
  [architecture.md](architecture.md#nvs--the-nvs_flash-c-api-used-directly)).
  **We never read the host's `~/.android/adbkey`** — always generate/store our
  own key in NVS.
- `transport` — the byte pipe under the protocol engine.
- `adb_connection`/`adb_stream` — the CNXN/AUTH handshake and `A_OPEN/OKAY/
  WRTE/CLSE` stream multiplexing. The packet read loop (`run_blocking()`) is
  driven by whatever thread the *caller* provides (a `std::thread` in host
  tests, a FreeRTOS task in `adb::Client`) — the engine only needs a
  thread-safe `send()` + stream registry, so it stays thread-model-agnostic.
  Teardown closes any still-open streams so every owner gets exactly one
  terminal callback.

## Transport: USB (device/simulator split) vs TCP (shared)

USB bulk transfer to the ADB interface (class `0xFF`/subclass `0x42`/protocol
`0x01`) is the **only** device/simulator split in this component:
`transport_usbhost.cpp` (esp-idf `usb_host`, async API wrapped as a synchronous
`Transport` with a binary semaphore per direction) vs `transport_libusb.cpp`
(libusb) — same `ESP_PLATFORM`-branch pattern as `m5stack-bsp`. It lives inside
`embedded_adb`, not the BSP, because the `usb_host` API is too large to
reimplement on the host and the need (claim interface, open bulk endpoints,
transfer) is ADB-specific, not a generic board seam. See
[gotchas.md](gotchas.md#usb-host-componentsembedded_adbsrctransport_usbhostcpp)
for the FIFO-bias, RX-starvation, VBUS-enumeration and teardown pitfalls this
transport had to work around.

**VBUS is the app's concern, not `embedded_adb`'s.** The library only knows
"reset the port" (`adb::set_usb_host_reset_hook`, called once after
`usb_host_install`); what a reset *is* — a `USB5V_EN` power-cycle via
`bsp_usb_host_set_power()` — and when VBUS is on at all (the persisted
**USB Power** setting: Always-on so a plugged phone charges, vs
Connected-only) both live in `adb_app.cpp`. No `embedded_adb → bsp` dependency.

`transport_tcp.cpp` — **ADB-over-TCP** — is the one transport shared verbatim
between both targets (lwip on device, BSD sockets on the simulator): ADB's wire
protocol is identical over a socket, and sockets are a standard contract, not
an Espressif API, so no `idf_compat` shim either. Two flavours:
1. **classic `adb tcpip 5555`** — same RSA AUTH as USB.
2. **Android 11+ wireless debugging** — the device replies `A_STLS`
   (STARTTLS) and the link upgrades to **TLS 1.3** before the banner exchange.
   `Transport::start_tls()` presents a self-signed X.509 cert built from the
   same adb RSA key (fixed wide validity, since the device has no RTC either)
   and runs mutual TLS with `authmode NONE` (we don't verify the device's
   ephemeral cert; adbd authenticates *us* by the client cert). **No RSA AUTH
   challenge follows** — the cert *is* the auth, and a key already authorized
   over USB is accepted for wireless TLS with no separate pairing step.

TCP throughput tuning (lwip window sizing) and the mbedTLS TLS1.3/serial-write
gotchas are in [gotchas.md](gotchas.md#wi-fi--esp-hosted-componentswifi-esp32p4sdkconfigdefaults).
The remaining ADB-layer throughput ceiling is the classic per-`A_OKAY`
stop-and-wait (one `A_WRTE` per RTT) — removing it needs the `delayed_ack`
feature (banner feature + `A_OPEN.arg1`=window + a 4-byte acked-bytes field in
`A_OKAY`), not implemented.

## `adb`'s generic raw stream — why it exists

Beyond `Shell`/`Sync`, `adb` exposes a generic, service-agnostic
`Client::open_stream(service, listener) -> shared_ptr<adb::Stream>`
(`adb_raw_stream.hpp`) — essentially `Shell` minus the PTY semantics. Its
reason to exist is **dependency direction**: app-specific protocols in other
components (`agent_link`) build on this generic stream so they depend on
`adb`, never on `embedded_adb` directly, and the generic `adb::Client` never
grows knowledge of any one app protocol (no `open_agent_link()` on `Client`).

## App-level integration (`adb_app.cpp`)

The connection holder is a small app-global that owns the single
`shared_ptr<adb::Client>` (must outlive the transient screens) and implements
`adb::ClientListener` — its only job is marshalling reader-thread `on_state`
callbacks onto the LVGL thread. `app::connection_transport()`
(`Transport::{Usb,Tcp}`) is the general-purpose hook for any feature that must
branch on the link kind — the mirror screen uses it to drop JPEG quality/bump
strip count over the slower TCP/Wi-Fi link (see [agent.md](agent.md)), and the
Wi-Fi power-save policy uses it too (see `components/wifi/README.md`).

Test running for both components is covered in
[development.md](development.md#host-test-runners); the dev loop for the
protocol/auth/stream layers runs against a **real phone plugged into the PC**
via the simulator's libusb transport, so only `transport_usbhost.cpp` needs
on-device validation.
