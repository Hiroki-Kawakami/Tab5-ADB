# `Stream` — generic raw service stream

`adb::Stream` is the **untyped building block** under the typed sessions. Where
`Shell` knows `shell:` PTY semantics and `Sync` knows the `sync:` sub-protocol, a
`Stream` just `A_OPEN`s an arbitrary service (the caller passes the service
string) and exposes bidirectional bytes with no framing imposed.

```cpp
auto stream = client->open_stream("localabstract:tab5adb-agent", listener);
//   -> std::shared_ptr<adb::Stream>, or nullptr if the client is not Online
```

See [`../inc/adb_raw_stream.hpp`](../inc/adb_raw_stream.hpp). The cross-cutting
rules (threading, the `self` arg, weak-listener lifetime, `Error`) are in the
component [`README.md`](../README.md); this surface follows the **session**
archetype.

## Why it exists — dependency direction

`Stream` is the seam that lets app-specific protocols live in their **own**
components while still depending only on `adb`. The motivating consumer is
[`agent_link`](../../agent_link) (the Tab5 side of the `tab5adb-agent` wire
protocol): it builds framing / HELLO / video on top of a `Stream`, so the
dependency arrow is `agent_link` → `adb` → `embedded_adb`. The generic
`adb::Client` must **not** grow a `open_<that-protocol>()` method — that would
invert the arrow (the generic layer depending on the app-specific one). The entry
point for each such protocol lives in *its* component and takes the `Client`.

`embedded_adb`'s `AdbConnection::open_stream` is already service-agnostic, but it
is the engine layer; `adb::Stream` is the **app-facing** wrapper that adds the
connection-lifecycle ownership and the non-blocking writer task, matching the rest
of the component.

## Surface

| Member | Notes |
|---|---|
| `Error write(data, len)` / `write(string)` | Non-blocking, any thread. Enqueues to a per-stream writer task that owns the blocking `AdbStream::write()` (one `A_WRTE` per `A_OKAY`). Returns `Ok` / `StreamClosed` / `QueueFull` (backpressure cap ≈ 64 KB). |
| `bool is_open()` | The underlying stream has reached open and is not closed. |
| `void close()` | `A_CLSE` and join the writer task. Idempotent; from the reader thread (e.g. inside `on_stream_close`) it only signals. |

`StreamListener` (both on the reader thread, `Stream*` first arg):

| Callback | Notes |
|---|---|
| `on_stream_data(Stream*, data, len)` | Peer bytes, exactly as sent — **no framing**. A consumer that needs frames accumulates and parses them itself (e.g. `agent_link`'s reactive parser). |
| `on_stream_close(Stream*, Error)` | Fires exactly once; no data follows. `Ok` in v1. |

## Relationship to `Shell`

`Stream` is essentially `Shell` minus the `shell:` service string and PTY
semantics — same writer-task model, same weak-listener lifetime, same
exactly-once close. `Shell`/`Sync` are kept as their own typed surfaces (their
services have real sub-protocol/semantics worth typing); `Stream` is the escape
hatch for everything else, including protocols owned by other components.
