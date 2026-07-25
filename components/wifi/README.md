# wifi — Wi-Fi STA connection manager

The app-facing component that gets the Tab5 onto a Wi-Fi network. It is the
prerequisite for **ADB-over-TCP** (the eventual "Wireless (TCP/IP)" connect path):
once an IP is up, the ADB TCP transport is a plain socket and lives in
the `adb` component's private transport, independent of this component.

This is the front door. The single public surface is `wifi::Manager` (see
`inc/wifi_manager.hpp`); the per-surface contract detail is in `docs/wifi.md`.

## What it owns

- **The Wi-Fi STA lifecycle.** `start()` brings up the network stack
  (`esp_netif` / `esp_event` / `esp_wifi` on device) and the radio. This is
  standard ESP-IDF lifecycle, deliberately **not** in the BSP — the BSP only
  declares the board's esp-hosted wiring via sdkconfig (see below).
- **Scan / connect / disconnect / forget**, with the last network persisted to
  NVS so the UI can show it and `connect_saved()` can reconnect.
- **Radio on/off** (`set_enabled()`/`enabled()`) — the Wi-Fi enable switch:
  `esp_wifi_stop`/`start` keeping the initialized stack; Off moves to `State::Off`.
- **Boot auto-connect** (`autoconnect_saved()`) — radio On + connect to the saved
  network, run from a task at startup; `set_listener()` registers a status listener
  without the blocking bring-up.
- **State reporting**: one-shot completions for `scan()`/`connect()` plus a
  persistent `Listener` for steady-state transitions (incl. a mid-session drop —
  the thing the reference NetworkManager couldn't report).

## On the Tab5 (ESP32-P4): esp-hosted

The P4 has no radio; Wi-Fi is an **ESP32-C6 co-processor** reached over SDIO via
`esp-hosted` + `esp_wifi_remote`. `esp_wifi` calls are API-compatible and routed
to the C6; `esp_wifi_init()` brings the SDIO transport up itself. The wiring
(SDIO pins, reset GPIO, slave target = esp32c6) is **sdkconfig**, not C code —
see `esp32p4/sdkconfig.defaults` (mirrors the `tab5remote` reference project). So
this component is plain `esp_wifi` with no board-specific bring-up.

## Layering & the device/simulator split

The component is portable C++ (`wifi_manager.cpp`: state machine, NVS, timeout,
listener dispatch) over **one** device/simulator backend split — the same shape
as `adb`'s private USB transport:

```
inc/wifi_manager.hpp   public API (Manager, Listener, Status, AP, Result)
inc/wifi_sim.hpp       simulator-only fake control (no-op/absent on device)
src/wifi_backend.hpp   internal Backend / BackendHost seam
src/wifi_manager.cpp   portable Manager (both targets)
src/backend_espwifi.cpp  device: esp_wifi / esp_netif / esp_event
src/backend_sim.cpp      simulator: deterministic scriptable fake
```

`esp_wifi` is too large to reimplement on the host (the same reason the project
keeps `usb_host` as a transport split rather than an `idf_compat` shim), and
simverify must stay deterministic (no real host-network access), so the simulator
gets a **scriptable fake** rather than a real esp_wifi shim. The fake returns a
canned AP list and scripted connect outcomes, driven by `wifi::sim::*`
(`wifi_sim.hpp`) — the sim harness exposes these as `wifi-*` script commands, and
`SIMULATOR_WIFI_CONNECT` sets the outcome for non-scripted headless runs.

## Cross-cutting contract

- **Threading.** All callbacks — one-shot completions *and* the persistent
  `Listener` — fire on the backend's event thread, **never the LVGL thread**.
  Marshalling to LVGL is the app's job (`lv_async_call`), exactly like `adb`.
  Query methods (`status()`/`configured()`/...) are callable from any thread.
- **Listener lifetime.** Held as a `weak_ptr`; drop its `shared_ptr` to detach
  (no `detach()` call), Shell/Sync-style.
- **One-shot vs persistent.** `connect()`/`scan()` completions fire exactly once.
  A later steady-state drop is reported through the `Listener`, not the (already
  fired) connect completion.

## Singleton

`wifi::manager()` returns the process-wide instance — a deliberately leaked
heap object (like `app::agent_client()`), so exit-time static destructors that
call in don't race a destroyed mutex.

## Status / roadmap

- **Done:** component scaffold, portable Manager, device esp_wifi backend, sim
  fake backend, simverify wiring. Builds on both targets.
- **Next:** Wi-Fi UI from the HomeScreen Wireless card; real-HW bring-up on the
  Tab5 (verify the C6 esp-hosted link + sdkconfig pins); then the `adb`
  TCP transport + `adb::Client::connect_tcp()`.
