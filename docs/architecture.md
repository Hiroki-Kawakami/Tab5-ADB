# Architecture

## Component layout

```
app/                         # SHARED  app logic / screens (a single component)
  terminal/                  #   ADBShellScreen's terminal widgets: term_view + term_keyboard
  agent/                     #   embedded tab5adb-agent jar (agent_jar.{h,c}, xxd -i of the built jar)
  test/                      #   host unit tests (parsers) + run.sh — no phone, no LVGL
components/                  # SHARED  (both targets)
  m5stack-bsp/                 board support (bsp_*) — see bsp.md
  lvgl++/                      C++ helpers over LVGL (lv_async_call, event fn wrapper — see gotchas.md)
  screen_manager/               Screen base + screen stack
  embedded_adb/  adb/           ADB host-side client — see adb.md
  agent_link/                   Tab5-side link to tab5adb-agent — see agent.md
  wifi/                         Wi-Fi STA connection manager — see components/wifi/README.md + docs/wifi.md
  jpeg_decode_enhanced/         enhanced P4 HW JPEG decode — see its README.md
  term_emu/                     VT100/xterm-subset terminal emulator (no LVGL/adb deps)
esp32p4/                     # DEVICE build root (IDF project) — main/ is app_main + esp_lvgl_port
simulator/                   # SIMULATOR build root
  platform/                  #   SIM-only entry (main.cpp: SDL/LVGL timer loop, sim_harness.cpp)
  idf_compat/                #   SIM-only ESP-IDF compat component — see its README.md
android-agent/               # ANDROID  tab5adb-agent (scrcpy-style app_process server) — see its README.md
```

Rule of thumb: **shared → top-level `components/` (or `app/`); target-only →
under that target's build root.** No build file reaches into another tree.

Every shared component with non-trivial docs owns a `README.md` (front door) and
a `docs/<surface>.md` per API surface (`components/adb/`, `components/wifi/`).
`m5stack-bsp`, `embedded_adb`, `agent_link`, `term_emu`, `screen_manager` and
`lvgl++` don't have their own docs yet — their design is covered here instead
(`bsp.md`, `adb.md`, `agent.md`).

## Components are self-describing (`idf_component_register`)

Every shared component owns one `CMakeLists.txt` that declares its sources /
includes / deps with the ESP-IDF `idf_component_register()` call — the single
source of truth consumed by both builds:

- **Device:** `esp32p4/CMakeLists.txt` sets
  `EXTRA_COMPONENT_DIRS = ../components ../app`. The IDF `main` component
  implicitly depends on all others, so `esp32p4/main` needs **no `REQUIRES`**
  (adding one suppresses that implicit dep and breaks transitive BSP/LCD
  includes).
- **Simulator:** `simulator/CMakeLists.txt` defines a small `idf_component_register`
  **shim** (a CMake function) and `add_subdirectory`s each component; the shim
  folds the component's `SRCS`/`INCLUDE_DIRS` straight into the `simulator`
  executable. `REQUIRES` are ignored (one binary → includes are global). Both
  the shared components and the simulator-only `idf_compat` component are
  consumed this way — growing `idf_compat` just means adding `SRCS` to its own
  `CMakeLists.txt`. Only `simulator/platform/` (the SDL/LVGL entry) stays a
  direct `target_sources` — it's the executable's "main", not a component. A
  shared component that builds differently per target (like `m5stack-bsp`)
  branches on `ESP_PLATFORM` inside its own `CMakeLists.txt`; guard anything
  that touches `${COMPONENT_LIB}` (e.g. `-flto`) in the device branch, since the
  shim doesn't define it.

## Where a device/simulator-divergent API goes — the rule

When an API needs different device vs simulator implementations, decide by one
question: **does Espressif already define this API?**

- **Yes** (`esp_err`, `esp_log`, `nvs_flash`, FreeRTOS, `esp_timer`,
  `esp_heap_caps`, the JPEG/PPA driver APIs) → implement the *ESP-IDF API
  itself* on the host in `simulator/idf_compat/`. Don't re-abstract something
  already abstracted; app code stays standard ESP-IDF on both targets.
- **No** — it's this board's own hardware concern with no standard contract
  (framebuffer, touch point, brightness, USB host, Wi-Fi radio bring-up) →
  either put it behind the BSP (if it's truly generic hardware I/O — display,
  touch, audio, SD) or give the owning component its own device/simulator
  backend split (`embedded_adb`'s USB transport, `wifi`'s backend seam) when
  the API is too large/specific to reimplement on the host. Keep any such
  surface small and demand-driven; don't pre-build a speculative one.

Ambiguous cases resolve cleanly under this rule: `nvs_flash` is an ESP-IDF API
→ `idf_compat`, app code calls the C API directly (see NVS below), **not** a
BSP seam. `esp_wifi` is Espressif's API but far too large to reimplement on the
host, so `wifi` gets its own backend split instead of an `idf_compat` shim
(same reasoning as the USB host transport).

## App entry & the LVGL runtime (not in the BSP)

`bsp_*` covers hardware; the LVGL **runtime** (the task/loop that drives
`lv_timer_handler`) is a per-target runtime concern and stays out of the BSP:

- Device: `esp32p4/main/main.cpp` `app_main()` starts esp_lvgl_port
  (`lvgl_port_init`) then calls `adb_app()`.
- Simulator: `simulator/platform/main.cpp` `main()` runs `lv_init`, sets the LVGL
  tick/delay to SDL, then calls `adb_app()` and runs the `lv_timer_handler` loop
  on the main thread (the one main-thread rule — see
  [FreeRTOS on the host](#freertos-on-the-host)).

`adb_app()` in `app/adb_app.cpp` is the shared entry: `bsp_init()` →
`DisplayManager::init()` (owns the LVGL display, the touch indev, and the
mirror overlay compositor — see [bsp.md](bsp.md#displaymanager-touch-input)) →
push the first screen. Panel is 720×1280 portrait (`PANEL_W`/`PANEL_H` in
`app/adb_app.hpp`). The pixel format is chosen once at `bsp_init` and fixed for
the boot — RGB888 by default. **RGB888 framebuffers hold LVGL's native B,G,R
byte order**; every framebuffer writer (the DisplayManager overlay's PPA
565→888 composite, the mirror's JPEG decode `BGR` rgb_order, the sim's
`SDL_PIXELFORMAT_BGR24` texture) agrees on it, keyed off
`bsp_display_get_pixel_format()` — that's the one switch if the boot format
ever changes.

## NVS — the `nvs_flash` C API, used directly

NVS is the worked example of the device/simulator-divergent-API rule above: the
seam is the ESP-IDF `nvs.h`/`nvs_flash.h` **C API** itself, so shared code calls
that C API directly on both targets — device gets the real flash-backed
component, the simulator gets a JSON-backed compat impl in
`simulator/idf_compat/` (file defaults to `nvs_data.json` in the cwd; override
with the sim-only `nvs_flash_sim_set_path()` before the first open). Keep NVS as
the C API on both sides — a C++ convenience layer, if wanted, belongs in a
shared component on top of the C API, not as a per-target file.

## System clock set from the phone (`app::sysclock`)

The Tab5 has **no battery-backed RTC**, so the system clock starts unset every
boot. `app/sysclock.{hpp,cpp}` sets it once per adb link from the connected
phone (`Holder::on_state(Online)` in `adb_app.cpp` runs `exec("date +'%s %z'")`
fire-and-forget on the adb reader thread; a parse failure just leaves the clock
unset), so captures/logs get real dated filenames
(`dated_path(dir, prefix, ext)`) instead of an RTC-less sequence number. The
pure parsing/formatting functions (`parse_date_z`, `posix_tz_from_offset`,
`format_stamp`) are host-tested in `app/test/test_sysclock.cpp` with no I/O
dependency. `posix_tz_from_offset`'s sign is **inverted** vs ISO (+0900 →
`UTC-9`) — that's the POSIX `TZ` convention, not a bug.

This is not an `idf_compat` shim (`settimeofday`/`setenv` are plain libc, not
an Espressif API) and not a BSP seam (no hardware behind it) — just a small
shared app-layer module, like `device_info`/`apk_info`.

## FreeRTOS on the host

The simulator does **not** run the FreeRTOS kernel. The FreeRTOS *API contract*
is reimplemented on native pthreads in `simulator/idf_compat/`
(`include/freertos/*.h` + `src/freertos_*.c`) — the same "reimplement the
contract, don't port the implementation" approach as the `esp_*` shims. A task
**is** a detached pthread scheduled by the host OS; there's no scheduler to
start, no tick ISR.

This is deliberate: the host OS already schedules, so emulating a
single-core, signal-driven FreeRTOS scheduler on pthreads would buy fidelity
this UI/app-logic simulator doesn't need, at the cost of fragile constraints
(SDL must own the main thread; `vTaskStartScheduler()` blocks forever; tasks
spawnable only post-boot; macOS/arm64 port bugs).

What this means for app code:
- **`xTaskCreate()` works anywhere, anytime** — including directly from
  `adb_app()`/`bsp_init()`. No "spawn only post-boot", no `lv_async_call` dance
  for non-LVGL FreeRTOS work. `app_main()` (device) and `main()` (sim) both just
  call `adb_app()`.
- **The one semantic gap:** real hardware runs one task at a time per core, so a
  critical section is atomic against all other tasks. With pthreads, tasks run
  truly in parallel, so `taskENTER_CRITICAL`/`portENTER_CRITICAL` map to a
  single global recursive mutex (a "big kernel lock") that restores
  single-at-a-time semantics *inside* critical sections but doesn't serialize
  ordinary task code. Priorities are stored but not enforced; `*FromISR`
  variants forward to their blocking counterparts (no ISRs on the host).
- Tick rate matches the device (`configTICK_RATE_HZ = 100`), so
  `pdMS_TO_TICKS()` behaves the same on both targets.

**SDL/LVGL own the main thread** (`SDL_PollEvent`/Cocoa on macOS), so
`simulator/platform/main.cpp` runs `lv_timer_handler` there — the *only*
main-thread rule, independent of task creation. LVGL is single-threaded on both
targets, so a FreeRTOS task that touches LVGL marshals via `lv_async_call`
(device: `lvgl_port_lock`) either way.

## Other simulator notes

- LVGL config is `simulator/lv_conf.h` (via `LV_CONF_INCLUDE_SIMPLE`).
  `LV_COLOR_DEPTH 16` (RGB565, matches the panel).
- `simulator/idf_compat/README.md` is the source of truth for the host compat
  surface (layout, include-path rules, how to add a shim). Current surface:
  `esp_err`/`esp_log`/`esp_check`/`esp_timer`/`esp_heap_caps`, `nvs`/`nvs_flash`,
  `driver/jpeg_decode` (libjpeg-backed), `driver/ppa` (CPU software impl), and
  `freertos`.
- Build artifacts (`build/`, `simulator/.deps/`) are gitignored. LVGL is fetched
  into `.deps`.
