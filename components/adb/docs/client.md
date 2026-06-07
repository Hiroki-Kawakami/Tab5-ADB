# `Client` — one connected device (and the factory)

`Client` is the entry point of the `adb` component: one instance represents one
connected device. It owns the whole connection lifecycle — the RSA identity, the
USB transport, and the internal reader task that runs the CNXN + AUTH handshake
and the packet read loop on top of `embedded_adb`'s `AdbConnection`. It is also
the factory for every session (`Shell`, `Sync`) and one-shot (`screencap`,
`exec`).

> Read the [cross-cutting rules](../README.md#cross-cutting-rules) first —
> threading, the `self` first argument, lifetime, and `adb::Error` apply here and
> are not restated below.

## API

```cpp
class ClientListener {
public:
  virtual ~ClientListener() = default;
  // State transitions (deduplicated). banner() is valid once Online.
  virtual void on_state(Client* c, ConnectionState state) = 0;
};

class Client {
public:
  // Async connect to the first USB Android device: load/create the RSA identity,
  // open the transport, run CNXN+AUTH on an internal reader task. Returns
  // immediately; progress arrives via ClientListener::on_state. `listener` must
  // outlive the Client.
  static std::shared_ptr<Client> connect_usb(ClientListener* listener);

  ConnectionState    state()  const;   // current state (thread-safe)
  const std::string& banner() const;   // device banner, valid once Online

  // --- sessions (return shared_ptr) ---  [planned]
  std::shared_ptr<Shell> open_shell(ShellListener*, const std::string& cmd = "");
  std::shared_ptr<Sync>  open_sync (SyncListener*);

  // --- one-shots (std::function completion) ---  [planned]
  void screencap(std::function<void(Error, const uint8_t* png, size_t n)> cb);
  void exec(const std::string& cmd,
            std::function<void(Error, const std::string& output)> cb);

  // Stop the reader task, close all sessions, cancel all in-flight one-shots
  // (each gets Error::Cancelled). Idempotent. Blocks until the reader task has
  // exited so no callback fires after it returns — EXCEPT when called from within
  // a callback on the reader thread, where it only signals (no self-join).
  void close();
};
```

## Connection states

`ConnectionState` is reused verbatim from `embedded_adb`:

```
Offline → Connecting → Authorizing → Unauthorized → Online        (or → Closed)
```

`on_state` is **deduplicated** (a state is reported at most once per transition).
`Unauthorized` means the device is showing "Allow USB debugging?"; the flow
proceeds to `Online` once the user taps Allow (the key is then persisted in NVS,
so subsequent connects skip the prompt). `Closed` is terminal — reached on
transport error or after `close()`.

## Lifetime notes specific to `Client`

- `connect_usb()` returns immediately; the connection is established on the reader
  task. The caller holds the `shared_ptr`; the reader task keeps the `Client`
  alive internally, and `close()` (called explicitly or from the destructor)
  joins the task before returning.
- `close()` is safe to call from within `on_state` (it runs on the reader thread
  and would otherwise self-join → deadlock); in that case it only signals the
  stop and returns.
- Teardown is race-free even if `close()` lands during key generation / USB
  enumeration or in the window before the read loop starts: it relies on
  `AdbConnection::stop()` being honored before `run_blocking()` begins (a
  `stop_requested_` gate in `embedded_adb`).

`Client` replaces the connection-setup half of the provisional
`app/adb_session.cpp`; once the UI is rewired onto it (slice 2), `adb_session.cpp`
is retired.

## Test

`test/test_client.cpp` is the host harness for this surface (libusb against a
real phone, same pattern as `embedded_adb/test`): it `connect_usb()`s, waits for
`Online`, checks `state()`/`banner()`, then `close()`s and confirms a clean
`Closed`. Build/run instructions are in the file header.
