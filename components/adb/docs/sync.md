# `Sync` — `sync:` filesystem session

`Sync` is a **session**: a long-lived `sync:` stream multiplexed over the
connection, the channel ADB uses for file transfer and metadata. Unlike `Shell`
(a byte pipe to a PTY), the `sync:` stream carries a small request/response
sub-protocol — `STAT`, `LIST`, `SEND`, `RECV` — and only **one request runs at a
time** on the stream. So `Sync` is a session whose *methods are one-shot style*:
you open one `Sync` per device and issue `stat()` / `push()` / … on it, each
taking a `std::function` completion (the closure is the correlation; no tag).

> Read the [cross-cutting rules](../README.md#cross-cutting-rules) first —
> threading, the `self` first argument, lifetime, and `adb::Error` apply here.
> The one **threading refinement** for `Sync` is called out below.

## Status

Built direction-by-direction (each direction is its own UI later):

| Op | Direction | Status |
|---|---|---|
| `push` (SEND) | **Tab5 → Android** | implemented |
| `stat` (STAT) | metadata (verifier for push) | implemented |
| `list` (LIST) | directory browse | planned (next) |
| `pull` (RECV) | **Android → Tab5** | planned |
| preview | Android → memory | planned (built on `pull`) |

## API

```cpp
class Sync;

// st_mode/size/mtime of a path. mode == 0 means the path does not exist
// (the device's lstat failed) — there is no separate "not found" error.
struct FileStat {
  uint32_t mode = 0;   // unix st_mode; 0 = does not exist
  uint32_t size = 0;   // bytes (v1 is 32-bit; >4 GiB needs STAT_V2, later)
  uint32_t mtime = 0;  // seconds since epoch
  bool exists() const { return mode != 0; }
  bool is_dir()  const { return (mode & 0170000) == 0040000; }
  bool is_reg()  const { return (mode & 0170000) == 0100000; }
};

class SyncListener {
public:
  virtual ~SyncListener() = default;
  // The sync session ended (our close(), the peer, or Client::close() teardown).
  // Fires exactly once, on the worker thread. No completion fires after it.
  virtual void on_sync_close(Sync* s, Error err) = 0;
};

// Pull-style byte source for push(): fill up to `cap` bytes into `buf`, return
//   >0  bytes produced
//    0  end of file (stop)
//   <0  abort the transfer (push completes with Error::Transport)
// Called repeatedly on the Sync worker thread until it returns <= 0.
using SyncSource = std::function<int(uint8_t* buf, size_t cap)>;

class Sync {
public:
  // Metadata of one path (lstat semantics: a symlink is not followed).
  void stat(const std::string& path, std::function<void(Error, FileStat)> cb);

  // Tab5 -> Android. Streams `source` to `remote_path`, creating/truncating it
  // with permission bits `perm` (e.g. 0644) and setting its mtime to `mtime`
  // (epoch seconds; 0 = epoch). The source is pumped on the worker thread, so it
  // may block on slow storage without stalling the caller. Completion fires once.
  void push(const std::string& remote_path, uint32_t perm, uint32_t mtime,
            SyncSource source, std::function<void(Error)> cb);

  bool is_open() const;   // stream opened and not yet closed

  void close();   // end the session (A_CLSE), stop the worker. Idempotent.
  void detach();  // stop referencing the listener (see Lifetime)
};
```

Created by the factory on `Client`:

```cpp
// Open the device's sync service. Returns nullptr if the client is not Online.
std::shared_ptr<Sync> Client::open_sync(SyncListener* listener);
```

## The `sync:` sub-protocol (what push/stat do on the wire)

After `A_OPEN "sync:"`, every message on the stream is a little-endian
**8-byte header** — a 4-byte ASCII id + a 4-byte length/arg — optionally followed
by a payload. The ids live in
[`embedded_adb/inc/adb_sync_protocol.hpp`](../../embedded_adb/inc/adb_sync_protocol.hpp)
(pure wire format, no I/O — like `adb_protocol.hpp`). The framing / state machine
is the `adb` `Sync` session (it owns a thread; the engine does not).

- **`stat(path)`** → send `STAT` + path; read a 16-byte `sync_stat_v1`
  (`id,mode,size,mtime`). `mode == 0` ⇒ does not exist.
- **`push(path, perm, mtime, source)`** (SEND v1):
  1. send `SEND` + `"<path>,<st_mode>"` (the mode is `perm | S_IFREG`, decimal).
  2. for each chunk from `source`: send `DATA` + `<size>` + bytes. Each `A_WRTE`
     is capped to the negotiated `max_payload` (so a 64 KiB sync chunk is split
     to fit the device's 16 KiB CNXN maxdata — `send_write` does **not** split).
  3. send `DONE` + `<mtime>` (no payload).
  4. read the status: `OKAY` ⇒ `Error::Ok`; `FAIL` + message ⇒ `Error::Rejected`.

Classic ADB flow control still applies underneath (one `A_WRTE` per `A_OKAY`);
`AdbStream::write()` blocks on it, which is exactly why the writes run on the
worker thread and not the caller's.

## Threading — the one refinement vs. the cross-cutting rule

The cross-cutting rule says callbacks fire on the **connection reader thread**.
`Sync` is the documented exception: the `sync:` protocol is synchronous
request/response, so `Sync` owns a private **worker thread** that drives it
(issues requests, blocks for responses through an internal byte pipe fed by the
reader thread, pumps `source` for `push`). **Op completions and `on_sync_close`
fire on this worker thread**, not the reader thread.

What does *not* change: callbacks still never fire on the LVGL/UI thread, so the
app marshals to LVGL exactly as before (`lv_async_call`). The only exception, as
with `exec`: if the session is already closing when you call a method, its
completion fires synchronously on the caller's thread with `Error::Cancelled`.

## Lifetime

`Sync` follows the [session lifetime rules](../README.md#lifetime):

- The factory returns a `std::shared_ptr<Sync>`. Methods are non-blocking and
  callable from any thread; each enqueues one op to the worker.
- **Every completion fires exactly once** — on success, on failure, on
  `close()`, or with `Error::Cancelled` when `Client::close()` tears the
  connection down (queued-but-unstarted ops are drained with `Cancelled`). An
  op already in flight when the stream drops completes with `StreamClosed`.
- `on_sync_close` fires **exactly once**, after the last op completion.
- `close()` sends `A_CLSE` and joins the worker; idempotent, and safe to call
  from inside a completion (it runs on the worker thread and only signals there —
  no self-join). The destructor calls `close()`.
- Before destroying the listener, call `close()` **and** `detach()`. `detach()`
  stops referencing the listener; don't call it from within a callback.

## v1 limitations (intentional)

- **32-bit size/mtime** (`STAT_V1`/`SEND_V1`). Files > 4 GiB and the richer
  `STAT_V2` fields await a v2 upgrade.
- **`on_sync_close` reports `Error::Ok`** for every normal end (peer/our close);
  the cause is not distinguished yet — same deferral as `Shell`/`exec`.
- **No compression** (`brotli`/`lz4`/`zstd` sync flags) — plain `SEND_V1`.
- `list` / `pull` / preview are the next directions (see Status).

## Test

`test/test_sync.cpp` is the host harness (libusb against a real, already
authorized phone — same pattern as `test_shell.cpp`): `connect_usb()` → wait
`Online` → `open_sync()` → `push()` a small in-memory buffer to a temp path under
`/data/local/tmp` → `stat()` it back and assert `exists()` and matching `size`,
then `close()` and confirm a single `on_sync_close`. Build/run in the file header.
</invoke>
