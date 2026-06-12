# `adb` — app-facing ADB API

`adb` is the **app-facing**, object-oriented layer over `embedded_adb`. Where
`embedded_adb` is the protocol engine (wire format, CNXN/AUTH, raw stream
multiplexing) and is deliberately *thread-model agnostic*, `adb` is the layer the
application actually drives: it owns the connection lifecycle (RSA key, USB
transport, the reader task) and exposes typed service objects (`Shell`, `Sync`)
and one-shot operations (`screencap`, `exec`).

```
app/                 ── UI, marshals callbacks onto the LVGL thread
  └── adb            ── THIS component: Client + typed services, owns reader task
        └── embedded_adb   ── protocol engine (AdbConnection / AdbStream), thread-agnostic
```

`adb` lives in its own component (not folded into `embedded_adb`) so the protocol
engine keeps its `std::thread`-only host unit tests, while `adb` is free to pull
in the FreeRTOS API (`xTaskCreate`) for the reader task — which works on both
targets (real kernel on device, pthread-backed compat in the simulator). `adb`
shares `namespace adb` with `embedded_adb`; the high-level names (`Client`,
`Shell`, `Sync`) don't collide with the engine names (`AdbConnection`,
`AdbStream`, `Packet`).

## Docs layout

This README is the **front door**: the overview, the cross-cutting rules every
API obeys, and the roadmap. Per-surface detail lives in `docs/`, one file per
API surface, grown slice by slice as each is implemented:

| Surface | Detail | Status |
|---|---|---|
| `Client` — connect + lifecycle, the factory | [`docs/client.md`](docs/client.md) | implemented (slice 1) |
| `exec` / `screencap` — one-shots | [`docs/one-shots.md`](docs/one-shots.md) | `exec` implemented (slice 2); `screencap` planned |
| `Shell` — interactive shell session | [`docs/shell.md`](docs/shell.md) | implemented (slice 3) |
| `Sync` — filesystem session | [`docs/sync.md`](docs/sync.md) | `push`/`stat`/`list` implemented (slice 5) |
| `Stream` — generic raw service stream | [`docs/streams.md`](docs/streams.md) | implemented (slice 6) |

When you implement a surface, write its `docs/*.md` **before** the code and link
it here.

## Cross-cutting rules

These hold for **every** API in this component — the per-surface docs reference
them rather than restating them.

### Threading

- **All callbacks fire on the ADB reader thread**, never on the LVGL/UI thread.
  Marshalling to the UI thread (e.g. `lv_async_call`) is the **application's**
  responsibility. The library never touches LVGL.
- **Methods are non-blocking and callable from any thread** (notably the LVGL
  thread). `Shell::write()` enqueues to the reader thread and returns immediately
  with an `Error` — it never blocks the caller on ADB flow control. (The blocking
  primitive still exists one layer down in `embedded_adb`'s `AdbStream::write()`.)

### The `self` first argument

Every **session** callback takes the originating object as its first argument
(`Client*`, `Shell*`, `Sync*`). This lets a single listener instance serve
multiple objects — e.g. one screen handling several `Shell`s on one device, or
(future) several `Client`s for multiple devices. **One-shot** callbacks are
`std::function`s instead: the closure captures whatever context it needs, so no
`self` or correlation tag is required.

### Callback style by archetype

| Archetype | Examples | Callback style | `self` arg |
|---|---|---|---|
| **Session** (long-lived, interactive) | `Shell`, `Sync` | abstract-class listener (subclass it) | yes |
| **One-shot** (request → single result) | `screencap`, `exec`, each `Sync` op | `std::function` completion | no (closure) |

Rationale: a session has several callbacks (data/close) and persistent identity,
so an interface + `self` is natural; a one-shot has exactly one callback, so a
lambda is far more ergonomic and the closure is the correlation mechanism.
Filesystem fits both: the `sync:` stream is itself a **session**, but its
per-request *methods* are one-shot style.

### Lifetime

- Session objects are owned via `std::shared_ptr`, returned by the factory. The
  reader thread holds a strong ref while delivering a callback, so the object
  never dies mid-callback.
- The library holds the **listener** (the app object implementing the interface)
  as a `std::weak_ptr` and `lock()`s it before each dispatch. The app owns the
  listener's lifetime via a `std::shared_ptr`. Contract: call `obj->close()` to
  stop I/O; **dropping the listener's `shared_ptr` is enough to detach** — once
  the weak ref expires, no further callback references it. There is no `detach()`:
  the `lock()` skips the callback racelessly if the listener is gone, so the app
  only does its usual UI marshalling and never threads lifetime bookkeeping
  through the library. (Hand the listener in as a `shared_ptr`/`weak_ptr`; e.g. a
  screen passes a `shared_ptr` aliasing `shared_from_this()`.)
- **Every terminal callback fires exactly once** *if the listener is still alive*.
  `on_*_close` (sessions) and the one-shot completion `std::function` each run at
  most once — on success, on failure, or with `Error::Cancelled` when
  `Client::close()` tears things down. (A session's `on_*_close` is delivered
  through the weak listener, so it is skipped if the listener was already dropped;
  one-shot completions own their captures and always fire.)

### Errors — `adb::Error`

```cpp
enum class Error {
  Ok = 0,
  NotConnected,   // not Online yet, or already closed
  StreamClosed,   // operation on a stream the peer/we already closed
  QueueFull,      // non-blocking write backpressure limit hit
  Timeout,
  Rejected,       // the service refused to open
  Protocol,       // malformed packet / unexpected response
  Transport,      // USB / transfer-level error
  Cancelled,      // in-flight op aborted by Client::close()
};
const char* to_string(Error);
```

Synchronous methods return `Error`; async completions take it as the first
payload. Backpressure: `Shell::write()` returns `Error::QueueFull` once unsent
bytes exceed the per-stream cap (initial cap ≈ 64 KB; see implementation).

## Running the host tests

`test/` has headless harnesses run on the desktop against a real phone over
libusb (no GUI). Use the runner — it builds the whole stack (this component +
`embedded_adb` + the idf_compat freertos/nvs shims), runs `adb kill-server` so
libusb can claim the interface, and launches the test:

```sh
nix develop -c components/adb/test/run.sh                 # TEST=test_client (default)
TEST=test_shell nix develop -c components/adb/test/run.sh
TEST=test_sync  nix develop -c components/adb/test/run.sh
```

Build artifacts go to `test/build/` (gitignored). The phone must be authorized
(run `test_client` once and tap "Allow USB debugging?"). `embedded_adb` and
`agent_link` have the same `test/run.sh` convention.

## Roadmap (commit-sized slices)

1. **`Client::connect_usb` + `state()`/`banner()` + `close()`** — *done*.
   Owns the RSA key / transport / reader task. Verified in the simulator (libusb)
   against a real phone via `test/test_client.cpp`. Detail:
   [`docs/client.md`](docs/client.md).
2. Wire `app/` onto `Client` + the `exec` one-shot — *done*. The app-global holder
   (owns the `shared_ptr<Client>`, marshals the reader-thread `on_state` to LVGL)
   lives in `adb_app`, and the getprop path goes through `Client::exec` (no
   `AdbConnection` leaked from `adb`). Pulled the `exec` one-shot forward from
   slice 4 to do this cleanly: [`docs/one-shots.md`](docs/one-shots.md).
   (`AdbConnection` teardown closes outstanding streams so a one-shot completion
   fires exactly once.)
3. **`Shell`** — interactive `shell:` session (and `shell:<cmd>`) — *done*.
   `Client::open_shell(listener, cmd="")` returns a `shared_ptr<Shell>`;
   `ShellListener` delivers `on_shell_data`/`on_shell_close` on the reader thread,
   and non-blocking `write()` enqueues to an internal writer task that owns the
   blocking `AdbStream::write()` (one `A_WRTE` per `A_OKAY`) — keeping the engine
   thread-agnostic. Verified by `test/test_shell.cpp` (libusb vs a real phone).
   Detail: [`docs/shell.md`](docs/shell.md).
4. **`screencap`** — the remaining one-shot (PNG capture), same shape as `exec`.
   → extend `docs/one-shots.md`.
5. **`Sync`** — `sync:` filesystem session; methods one-shot style. Built
   direction-by-direction (each gets its own UI): **`push` (Tab5→Android) first**
   with `stat` as its verifier, then `list` (the browse UI), then `pull`
   (Android→Tab5, the copy-to-SD flow) — all *done*; preview (pull into memory)
   remains. `Client::open_sync(listener)` returns a
   `shared_ptr<Sync>`; the session owns a private worker thread that drives the
   synchronous `sync:` sub-protocol, so its completions fire on that worker
   thread (the one documented threading refinement — still never the UI thread).
   Verified by `test/test_sync.cpp` (libusb vs a real phone).
   Detail: [`docs/sync.md`](docs/sync.md).
6. **`Stream`** — generic, service-agnostic raw stream
   (`Client::open_stream(service, StreamListener) -> shared_ptr<Stream>`) — *done*.
   The untyped building block beside `Shell`/`Sync` (Shell minus `shell:` PTY
   semantics): non-blocking `write()` over a per-stream writer task,
   `on_stream_data`/`on_stream_close` on the reader thread. Exists so app-specific
   protocols in **other** components (e.g. `agent_link`) layer on `adb` without
   depending on `embedded_adb` directly. Exercised end-to-end by
   `components/agent_link/test/test_hello.cpp`. Detail:
   [`docs/streams.md`](docs/streams.md).
