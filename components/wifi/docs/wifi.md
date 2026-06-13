# wifi::Manager — surface detail

See `README.md` for the component overview and the cross-cutting contract
(threading, listener lifetime, the device/simulator split). This file is the
detail for the `wifi::Manager` API.

## State model

```
State::Off          start() not called, or radio turned off (set_enabled(false))
State::Disconnected up, idle (or after a drop / disconnect())
State::Connecting   a connect() is in flight (associating / awaiting IP)
State::Connected    associated AND has an IP (usable)
```

`Listener::on_wifi_state(Status)` fires on every deduplicated transition. The
user-visible edges are `Connecting` (on `connect()`) → `Connected` (on IP) or
`Disconnected` (on failure/drop). Association without an IP stays `Connecting`
(no separate edge).

`Status` is `{ state, ssid, ip, rssi }`. `ip` is valid only in `Connected`;
`rssi` is the live AP RSSI in `Connected`, else 0.

## Lifecycle — the worker task

The radio bring-up (esp_wifi over the SDIO C6 link) and on/off **block**, so the
Manager owns **one worker task + a command queue** (created in its constructor;
the singleton is leaked, so the task lives for the process). `start()`,
`set_enabled()` and `autoconnect_saved()` are **non-blocking** — they enqueue work
the worker runs **serially**, so two callers can't race `bring_up()` and the LVGL
thread never blocks. (This replaced ad-hoc per-call tasks, which raced `bring_up()`
when a screen toggle overlapped the boot auto-connect.)

```cpp
wifi::manager().set_listener(listener_weak);  // cheap, synchronous: NO bring-up
wifi::manager().start(listener_weak);         // set_listener + enqueue radio-On
wifi::manager().autoconnect_saved();          // enqueue: bring up + connect saved
```

`set_listener()` registers/replaces the persistent listener **synchronously,
without** the bring-up — for a screen that wants live status but must stay
responsive (the radio comes up via `start()` / `set_enabled(true)` /
`autoconnect_saved()`). `start()` is `set_listener()` + an enqueued radio-On; on
device the bring-up is where `esp_netif_init` / `esp_event_loop` / `esp_wifi_init`
/ `esp_wifi_start` happen — the lifecycle the BSP no longer owns. It does **not**
auto-connect. `autoconnect_saved()` is the boot path: enqueue "bring the radio up
(On) and connect to the saved network if any" (no-op otherwise), **without**
changing the registered listener (status flows to whoever is registered).

## Scan (one-shot)

```cpp
manager().scan([](wifi::Result r, std::vector<wifi::AP> aps) {
    // event thread; marshal to LVGL. aps deduped by SSID, strongest RSSI, desc.
});
```

A second `scan()` before the first completes supersedes it (the earlier callback
fires with `Failed` + an empty list).

## Connect (one-shot + persistence)

```cpp
manager().connect(ssid, pass, [](wifi::Result r) {
    // event thread. Ok, or ApNotFound/AuthFailed/AssocFailed/IpFailed/Timeout/Failed
}, /*timeout_ms=*/15000);

manager().connect_saved(cb);   // reconnect with NVS creds; Failed if none saved
```

Credentials are saved to NVS (namespace `wifi`) on the attempt, so `configured()`
/ `saved_ssid()` / `connect_saved()` work afterward. The connect completion fires
**exactly once**; a later drop is a `Listener` event, not a second completion. A
connect with no terminal event within `timeout_ms` completes `Timeout` (the
Manager disconnects the backend).

## Radio on/off (the enable switch)

```cpp
manager().set_enabled(false);            // radio off -> State::Off (no scan/connect)
manager().set_enabled(true, on_applied); // radio On -> State::Disconnected; cb on worker
bool on = manager().enabled();           // false == State::Off
```

`set_enabled()` is **non-blocking** (enqueues to the worker; On brings the stack up
itself if needed, so no prior `start()` is required) and idempotent. The optional
`done` callback fires on the **worker thread** once the op is applied (always, even
for a no-op toggle) — the UI uses it to re-enable the switch + rescan. It notifies
the `Listener` with the new state. Turning Off drops any live connection and
completes a pending `connect()` with `Failed`. On device it is `esp_wifi_stop()` /
`esp_wifi_start()` (the initialized stack is kept); the `esp_wifi_stop` disconnect
event is not read as a drop-to-idle (state stays Off).

## Disconnect / forget

```cpp
manager().disconnect();   // leave the AP, stay up/idle; notifies the Listener
manager().forget();       // clear saved creds (does not drop a live link)
```

## Result mapping (device)

`backend_espwifi.cpp` maps `wifi_event_sta_disconnected_t::reason`:
`NO_AP_FOUND`→`ApNotFound`; `AUTH_FAIL`/`AUTH_EXPIRE`/`*HANDSHAKE_TIMEOUT`→
`AuthFailed`; `ASSOC_FAIL`/`CONNECTION_FAIL`→`AssocFailed`; else `Failed`. A drop
*after* association but before IP maps to `IpFailed`.

## Simulator control (`wifi_sim.hpp`)

The fake decides a connect outcome from the SSID by default:
`fail-auth`→`AuthFailed`, `fail-notfound`→`ApNotFound`, `fail-assoc`→
`AssocFailed`, `timeout`→(no event, lets the timeout fire), else `Ok`. Override
with `wifi::sim::set_next_connect_result()` / `set_aps()` / `set_event_delay_ms()`
/ `set_ip()` / `drop_link()`. The sim harness surfaces these as `wifi-aps`,
`wifi-connect-result`, `wifi-delay`, `wifi-drop` script commands.
