# `Shell` — interactive shell session

`Shell` is a **session**: a long-lived `shell:` stream multiplexed over the
connection. Unlike the `exec` one-shot (which collects all output of a single
command and completes once), a `Shell` stays open so the app can feed keystrokes
and stream output continuously — the building block of an on-device terminal.

> Read the [cross-cutting rules](../README.md#cross-cutting-rules) first —
> threading, the `self` first argument, lifetime, and `adb::Error` apply here and
> are not restated below.

## API

```cpp
class Shell;

class ShellListener {
public:
  virtual ~ShellListener() = default;
  // Output bytes from the device, as adbd emits them (stdout+stderr merged).
  // An interactive PTY shell (empty cmd) does \n -> \r\n translation; a
  // shell:<cmd> run does not. Fires on the reader thread.
  virtual void on_shell_data(Shell* sh, const uint8_t* data, size_t len) = 0;
  // The stream closed — peer exit, our close(), or Client::close() teardown.
  // Fires exactly once, on the reader thread. After it, no more on_shell_data.
  virtual void on_shell_close(Shell* sh, Error err) = 0;
};

class Shell {
public:
  // Non-blocking, callable from any thread (notably LVGL). Enqueues `data` to
  // the internal writer task and returns immediately:
  //   Ok           — queued
  //   StreamClosed — close()d, or the peer already closed the stream
  //   QueueFull    — unsent bytes would exceed the per-stream cap (~64 KB)
  Error write(const uint8_t* data, size_t len);
  Error write(const std::string& s);     // convenience

  bool is_open() const;   // opened (first A_OKAY seen) and not yet closed

  void close();   // send A_CLSE, stop the writer task. Idempotent.
  void detach();  // stop referencing the listener (see Lifetime below)
};
```

A `Shell` is created by the factory on `Client`:

```cpp
// Open an interactive shell (empty cmd -> a PTY shell) or a single command
// (cmd = "ls -l" -> shell:ls -l). Returns nullptr if the client is not Online.
std::shared_ptr<Shell> Client::open_shell(ShellListener* listener,
                                          const std::string& cmd = "");
```

## Behaviour

- **Wire service:** `shell:` (empty `cmd`) opens an interactive PTY shell;
  `shell:<cmd>` runs one command without a PTY. Both deliver their bytes through
  `on_shell_data` exactly as adbd writes them — no client-side line buffering.
- **Output** arrives incrementally on the reader thread via `on_shell_data`.
  Feed it straight into a terminal emulator / text area; the library does no
  interpretation (ANSI escapes, `\r\n`, etc. pass through untouched).
- **Input** is sent with `write()`, which never blocks on ADB flow control. The
  bytes are queued and flushed by an internal **writer task**; the blocking
  primitive (`embedded_adb`'s `AdbStream::write()`, one outstanding `A_WRTE` per
  `A_OKAY`) runs there, off the caller's thread. This is why `Shell` lives in
  `adb` (free to spawn a FreeRTOS task) and not in the thread-agnostic engine.
- **Backpressure:** if the device stops acking and unsent bytes pile up past the
  per-stream cap (~64 KB), `write()` returns `QueueFull` instead of growing the
  queue without bound. The caller decides whether to retry or drop.
- **Pre-open writes** are fine: bytes written before the device's first `A_OKAY`
  sit in the queue and flush once the stream opens.

## Lifetime

`Shell` follows the [session lifetime rules](../README.md#lifetime):

- The factory returns a `std::shared_ptr<Shell>`. The reader thread holds a
  strong ref while delivering a callback, so the object never dies mid-callback.
- `close()` sends `A_CLSE` and joins the writer task; it is idempotent and safe
  to call from inside `on_shell_close` (it runs on the reader thread and only
  signals there — no self-join). The destructor calls `close()`.
- `on_shell_close` fires **exactly once** — on peer exit, on `close()`, or when
  `Client::close()` tears the connection down (the engine closes outstanding
  streams on teardown).
- Before destroying the listener, call `close()` **and** `detach()`. `detach()`
  synchronizes with the reader thread (it takes the same lock callback dispatch
  holds), so once it returns no further callback will reference the listener.
  Don't call `detach()` from within a callback (self-deadlock); detach from the
  thread that owns the listener.

## v1 limitations (intentional)

- **`on_shell_close` always reports `Error::Ok`.** There is no exit code (that
  needs `shell_v2`) and the close cause (normal exit vs. `Client::close()`
  cancellation) is not distinguished yet — the **exactly-once** guarantee already
  holds. Same deferral as the [`exec` one-shot](one-shots.md#v1-limitations-intentional).
- **No window-size / PTY control** (`shell_v2` `WINDOW_SIZE_CHANGE`), and stdout
  and stderr are merged. Both await a `shell_v2` upgrade.

## Test

`test/test_shell.cpp` is the host harness (libusb against a real, already
authorized phone — same pattern as `test_client.cpp`): it `connect_usb()`s,
waits for `Online`, opens an interactive shell, writes a couple of commands,
collects the output through `on_shell_data`, then `close()`s and confirms a
single `on_shell_close`. Build/run instructions are in the file header.
