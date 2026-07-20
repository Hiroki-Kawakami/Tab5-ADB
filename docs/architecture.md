# Architecture

## Component layout

```
app/                         # SHARED  app logic / screens (a single component)
  terminal/                  #   ADBShellScreen's terminal widgets: term_view + term_keyboard
  agent/                     #   embedded tab5adb-agent jar (agent_jar.{h,c}, xxd -i of the built jar)
  test/                      #   host unit tests (parsers) + run.sh — no phone, no LVGL
components/                  # PROJECT-SPECIFIC SHARED components (both targets)
  embedded_adb/  adb/           ADB host-side client — see adb.md
  agent_link/                   Tab5-side link to tab5adb-agent — see agent.md
  wifi/                         Wi-Fi STA connection manager — see components/wifi/README.md + docs/wifi.md
  term_emu/                     VT100/xterm-subset terminal emulator (no LVGL/adb deps)
esp-devkit/                   # git submodule: reusable cross-project infrastructure
  bsp/                          board support (bsp_*) — see bsp.md
  ui_framework/                 LVGL C++ helpers, Screen base + ScreenManager
  libs/image_framework/         streaming image decode/resize pipeline
  libs/jpeg_decode_enhanced/    enhanced P4 HW JPEG decode
  idf_compat/                   host ESP-IDF compatibility component
  sim_harness/                  scripted headless simulator driver
esp32p4/                     # DEVICE build root (IDF project) — main/ is app_main + esp_lvgl_port
simulator/                   # SIMULATOR build root
  platform/                  #   SIM-only entry (main.cpp: LVGL loop + sim_harness wiring)
android-agent/               # ANDROID  tab5adb-agent (scrcpy-style app_process server) — see its README.md
```

Rule of thumb: **project-specific shared → top-level `components/` (or `app/`);
reusable board/simulator/UI infrastructure → `esp-devkit`; target-only → under
that target's build root.** Changes intended for several firmware projects must
land in esp-devkit first, then this repository advances the submodule pointer.

Every shared component with non-trivial docs owns a `README.md` (front door) and
a `docs/<surface>.md` per API surface (`components/adb/`, `components/wifi/`).
The esp-devkit-owned surfaces are documented in that submodule; this repository
only documents its integration decisions and app-specific use of them.

## Components are self-describing (`idf_component_register`)

Every shared component owns one `CMakeLists.txt` that declares its sources /
includes / deps with the ESP-IDF `idf_component_register()` call. Both build
roots include `esp-devkit/devkit.cmake`, which keeps component discovery and
the simulator shim consistent across projects:

- **Device:** `devkit_idf_init(UI_FRAMEWORK COMPONENT_DIRS ../components ../app)`
  registers esp-devkit plus the project components and trims the build to
  `main`'s transitive dependency graph. `esp32p4/main` explicitly requires
  `app`; `app/CMakeLists.txt` carries the remaining reusable dependency graph.
  This explicit root edge is required because the `COMPONENTS main` trim turns
  off IDF's usual main-to-everything implicit dependency.
- **Simulator:** `devkit_simulator(BOARD tab5 ...)` supplies SDL/LVGL,
  `idf_compat`, `sim_harness`, the Tab5 BSP, UI/image libraries and the
  `idf_component_register` shim. `simulator/CMakeLists.txt` lists only this
  project's component directories and host-only libusb/mbedTLS/zlib links.
  `simulator/platform/main.cpp` remains the executable entry point.

## Where a device/simulator-divergent API goes — the rule

When an API needs different device vs simulator implementations, decide by one
question: **does Espressif already define this API?**

- **Yes** (`esp_err`, `esp_log`, `nvs_flash`, FreeRTOS, `esp_timer`,
  `esp_heap_caps`, the JPEG/PPA driver APIs) → implement the *ESP-IDF API
  itself* on the host in `esp-devkit/idf_compat/`. Don't re-abstract something
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

- Device: `esp32p4/main/main.cpp` `app_main()` starts the esp-devkit
  `ui_framework` LVGL port (`lvgl_port_init`) then calls `adb_app()`.
- Simulator: `simulator/platform/main.cpp` initializes the same port surface,
  calls `adb_app()`, registers project-specific harness commands, then runs
  `lvgl_sim_loop(sim_harness_frame)` on the main thread.

`adb_app()` in `app/adb_app.cpp` is the shared entry: `bsp_init()` → apply the
stored USB-host power preference through `BSP_POWER_SWITCH_USB5V` →
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
`esp-devkit/idf_compat/` (file defaults to `nvs_data.json` in the cwd; override
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
is reimplemented on native pthreads in `esp-devkit/idf_compat/`
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
- `esp-devkit/idf_compat/README.md` is the source of truth for the host compat
  surface (layout, include-path rules, how to add a shim). Current surface:
  `esp_err`/`esp_log`/`esp_check`/`esp_timer`/`esp_heap_caps`, `nvs`/`nvs_flash`,
  `driver/jpeg_decode` (libjpeg-backed), `driver/ppa` (CPU software impl), and
  `freertos`.
- Build artifacts (`build/`) are gitignored. CMake fetches LVGL into
  `build/_deps/`.
