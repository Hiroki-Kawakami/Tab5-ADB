# Tab5-ADB — Notes for Claude

ADB (Android Debug Bridge) client on M5Stack Tab5 (ESP32-P4). Scaffolded from
the `Tab5-UVC-Display` project (build system, BSP, LVGL infra) with a host
simulator modeled on `NameCardKnot`.

> **Keep the docs current.** When you change the build flow, hit a non-obvious
> gotcha worth remembering, add a target/board, or make a design decision that
> isn't derivable from the code, update the relevant file under `docs/` (or add
> a new one) in the same change — these are the handoff to the next session.
> Don't restate what the code already says; write only the *why*, the *why
> not*, and pointers to where to look. Don't record implementation plans or
> progress status here — `git log` is the record of what changed and when.
>
> **Keep `README.md` current too.** It is the short human-facing overview
> (build / flash / simulator commands, the layout tree, license). When a change
> makes it stale, update it in the same change — and don't duplicate `docs/`
> content into it or vice versa.

## Start here

- [`docs/development.md`](docs/development.md) — build/flash/monitor, the host
  simulator, headless UI verification (simverify), the Android agent build
  loop, and every component's host test runner.
- [`docs/architecture.md`](docs/architecture.md) — component layout, the
  component build system (`idf_component_register`, the device/simulator CMake
  split), the rule for deciding where a device/simulator-divergent API goes,
  app entry & the LVGL runtime, NVS, the system clock, FreeRTOS-on-the-host.
- [`docs/gotchas.md`](docs/gotchas.md) — non-obvious pitfalls that cost real
  debugging time (USB host FIFO/enumeration, Wi-Fi/esp-hosted, touch reset,
  JPEG/display, `app_process` Android quirks, LVGL). Check here before
  re-debugging something that looks like a vendor-stack bug.
- [`docs/bsp.md`](docs/bsp.md) — the `esp-devkit` BSP integration (display/
  touch/audio/SD, the DisplayManager touch callback).
- [`docs/adb.md`](docs/adb.md) — the ADB host-side client
  (`adb` public/private layering, `agent_link`, USB/TCP transport design).
- [`docs/agent.md`](docs/agent.md) — `tab5adb-agent` (the Android-side
  companion) and the Tab5-side mirror/media/input pipeline.
- [`docs/ui.md`](docs/ui.md) — `app/`'s screen navigation map and the
  cross-cutting UI patterns (screen lifecycle/threading, recycled lists, file
  transfer jobs).

Some components already own their own docs — read those first for API detail,
they take priority over anything said here:
- `components/adb/README.md` + `components/adb/docs/*.md` — the app-facing ADB
  API (`Client`/`Shell`/`Sync`/`Stream`) contract, owned by the
  `esp-adb-host` submodule.
- `esp-devkit/libs/wifi/README.md` — `wifi::Manager`.
- `esp-devkit/libs/jpeg_decode_enhanced/README.md` — the enhanced JPEG decode API.
- `android-agent/README.md` + `android-agent/docs/protocol.md` +
  `android-agent/docs/testing.md` — the agent process and its wire protocol.
- `esp-devkit/idf_compat/README.md` — the host ESP-IDF compat shim surface.

## The one rule that matters most

**Always run build/toolchain commands through `nix develop -c <cmd>`** — never
invoke `idf.py`/`cmake`/`esptool` directly. See
[`docs/development.md`](docs/development.md) for every command.
