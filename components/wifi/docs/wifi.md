# wifi::Manager — surface detail

See `README.md` for the component overview and the cross-cutting contract
(threading, listener lifetime, the device/simulator split). This file is the
detail for the `wifi::Manager` API.

## State model

```
State::Off          start() not called
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

## Lifecycle

```cpp
wifi::manager().start(listener_weak);   // once at boot; idempotent (re-sets listener)
```

`start()` brings up the stack + radio in STA mode and ensures NVS. It does **not**
auto-connect. On device this is where `esp_netif_init` / `esp_event_loop` /
`esp_wifi_init` / `esp_wifi_start` happen — the lifecycle the BSP no longer owns.

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
