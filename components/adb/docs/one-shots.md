# One-shots — `exec` (and `screencap`, planned)

A **one-shot** is a request → single result operation on `Client`. Unlike a
session (`Shell`, `Sync`) there is no persistent object: the call takes a
`std::function` completion, and the closure *is* the correlation (no `self` arg,
no tag). See the [cross-cutting rules](../README.md#cross-cutting-rules) for the
threading / lifetime / error contract — only the per-op specifics are below.

## `exec`

```cpp
using ExecCb = std::function<void(Error, const std::string& output)>;

void Client::exec(const std::string& cmd, ExecCb cb);
```

Runs a single shell command and collects its output. Internally it opens a
`shell:<cmd>` stream on the connection, accumulates everything the device writes
until the device closes the stream, then fires `cb` **once** with the collected
output.

- **Wire service:** `shell:<cmd>` (the v1 shell protocol). Output is stdout and
  stderr merged, as the device emits it; for a non-interactive command adbd does
  not allocate a PTY, so there is no `\n`→`\r\n` translation. Callers that care
  about line structure should split/trim themselves.
- **Non-blocking, any thread.** `exec()` returns immediately; it never blocks the
  caller on ADB I/O. Safe to call from the LVGL thread.
- **Completion fires on the reader thread, exactly once** (the `open_stream`
  callbacks run there). Marshal to the UI thread in the closure as usual.
  - The one exception to "on the reader thread": if the client is **not Online**
    (no connection, or already closing), `cb` is invoked synchronously on the
    caller's thread with `Error::NotConnected` and empty output — there is no
    stream/reader to defer to.

### v1 limitations (intentional)

`exec` is the minimal one-shot that the device-info screen needs; it will grow:

- **No exit code.** The completion delivers output only; the command's exit
  status is not reported (`shell_v2` is needed for that — a later refinement).
- **Outcome is `Error::Ok` once the stream opened**, even if the device closed it
  early or `Client::close()` tore the connection down mid-flight (you get
  whatever output was collected, often empty). Distinguishing `Rejected` /
  `Cancelled` here is deferred; the **exactly-once** guarantee already holds
  because connection teardown closes outstanding streams (see
  `AdbConnection::run_blocking`).

### Example (device-info getprops)

```cpp
client->exec(
    "getprop ro.build.version.release; getprop ro.build.version.sdk; "
    "getprop ro.product.manufacturer; getprop ro.serialno",
    [label](adb::Error err, const std::string& out) {
        std::string text = err == adb::Error::Ok ? format(out)
                                                  : "(failed to read properties)";
        lv_async_call([label, text]{ lv_label_set_text(label, text.c_str()); });
    });
```

One `exec` (commands chained with `;`) returns four newline-separated values,
which beats four separate streams. The completion runs on the reader thread, so
the label update is marshalled to LVGL.

## `screencap` (planned)

```cpp
void Client::screencap(std::function<void(Error, const uint8_t* png, size_t n)> cb);
```

Captures the screen as a PNG via the `exec:screencap -p` / framebuffer service.
Same one-shot shape (closure completion, fires on the reader thread, exactly
once). Not yet implemented.
