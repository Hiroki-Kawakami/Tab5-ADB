# Tab5-ADB — Notes for Claude

ADB (Android Debug Bridge) client on M5Stack Tab5 (ESP32-P4). Scaffolded from
the `Tab5-UVC-Display` project (build system, BSP, LVGL infra) with a host
simulator modeled on `NameCardKnot`.

> **Keep this file current.** When you change the build flow, the simulator
> architecture, the BSP surface (`bsp_*`), add a target/board, or hit a
> non-obvious gotcha worth remembering, update CLAUDE.md in the same change. It is
> the handoff to the next session.
>
> **Keep `README.md` current too.** It is the human-facing overview (build /
> flash / simulator commands, the layout tree). When a change makes it stale —
> moved/renamed paths, changed commands, a new top-level directory — update
> `README.md` in the same change. CLAUDE.md is the detailed handoff; README.md
> is the short public summary, so keep README concise and don't duplicate the
> deep gotchas here into it.

## Build environment

The ESP-IDF (v5.4.3) toolchain and the host-simulator tools (cmake, ninja, gcc,
SDL2, cjson) all come from a Nix flake. **Always run commands through
`nix develop -c <cmd>`** (or from inside a `nix develop` shell) — never invoke
`idf.py` / `cmake` / `esptool` directly.

Nix flakes only see git-tracked files. After adding new files, `git add` them
(staging is enough) or `nix develop` won't pick up `flake.nix` changes / the
tree, and the build can fail with "not tracked by Git".

## Targets

### Device — `esp32p4/` (ESP32-P4 / M5Stack Tab5)

```sh
nix develop -c idf.py -C esp32p4 build
nix develop -c idf.py -C esp32p4 flash monitor   # needs a TTY
./run.sh esp32p4                                  # same as flash monitor
```

When `idf.py monitor` can't attach (no TTY in a non-interactive shell), drive
the serial directly with esptool + PySerial. The second `/dev/cu.usbmodem*`
enumerator is the JTAG/console port used for flashing; **ESP32-P4 only prints
logs once after reset**, so capture during the boot sequence.

### Host simulator — `simulator/` (SDL2 + LVGL + pthread-backed FreeRTOS API)

Runs `app/` on the desktop so UI / app logic can be developed without a board.

```sh
./run.sh                # == ./run.sh simulator
# expands to:
# nix develop -c sh -c 'cmake --fresh -S simulator -B build -G Ninja && cmake --build build && ./build/simulator'
```

`run.sh simulator` only re-runs `cmake --fresh` when `build/` is missing; delete
`build/` to force a clean reconfigure (e.g. after editing `simulator/CMakeLists.txt`).

## Architecture

### Component layout — one directory per category

```
app/                         # SHARED  app logic / screens (a single component)
components/                  # SHARED  (both targets)
  m5stack-bsp/               #   board support (bsp_*) — device drivers + SDL sim backend
    inc/                     #     model-agnostic public API (bsp.h) + bsp_audio.h, bsp_types.h
    inc_private/             #     internal driver interfaces (bsp_display.h / bsp_touch.h vtables)
    src/                     #     shared dispatch: bsp_display.c/bsp_touch.c (+ audio_eq.c, device)
    devices/                 #     DEVICE reusable chip drivers (ili9881c/st7123/gt911/es8388/pi4io)
    simulator/               #     SIM reusable SDL backend (sdl_backend.cpp -> display/touch provider)
    boards/<model>/          #     per-model bring-up: <model>.c (device) + <model>_sim.cpp (sim)
  lvgl++/                    #   C++ helpers (lv_async_call, lv_obj_add_event_fn)
  screen_manager/            #   Screen base + screen stack
  embedded_adb/              #   ADB host-side client (C++) — usb_host vs libusb split
    inc/                     #     public API (embedded_adb.hpp, adb_protocol.hpp)
    src/                     #     protocol/crypto/keystore/connection/stream + transport_*
esp32p4/                     # DEVICE build root (IDF project)
  main/                      #   entry point (app_main + device LVGL runtime via esp_lvgl_port)
simulator/                   # SIMULATOR build root (see below)
  platform/                  #   SIM-only entry (main.cpp: SDL/LVGL timer loop)
  idf_compat/                #   SIM-only ESP-IDF compat component (see its README.md)
    include/  src/           #     host shims: esp_* (err/log/check/timer/heap/nvs)
                             #     + freertos/* (pthread-backed FreeRTOS API)
```

`m5stack-bsp` is the worked example of a target-divergent shared component: its
one `CMakeLists.txt` branches on `ESP_PLATFORM` (set only under ESP-IDF) to build
the device drivers + tab5 board on device and the SDL backend + tab5 sim board on
the host. No more device-only `esp32p4/components/`.

Rule of thumb: **shared → top-level `components/` (or `app/`); target-only →
under that target's build root.** Adding a shared component = a new dir under
`components/` with its own `CMakeLists.txt` (see below). No build file reaches
into another tree.

### Components are self-describing (`idf_component_register`)

Every shared component owns one `CMakeLists.txt` that declares its sources /
includes / deps with the ESP-IDF `idf_component_register()` call — the **single
source of truth** consumed by both builds:

- **Device:** `esp32p4/CMakeLists.txt` sets
  `EXTRA_COMPONENT_DIRS = ../components ../app` (`../components` is a container of
  components; `../app` is itself one component because it has a `CMakeLists.txt`).
  Device-only components in `esp32p4/components/` are auto-discovered. The IDF
  `main` component implicitly depends on all others, so `esp32p4/main` needs **no
  `REQUIRES`** (adding one suppresses that implicit dep and breaks transitive
  BSP/LCD includes).
- **Simulator:** `simulator/CMakeLists.txt` defines a small `idf_component_register`
  **shim** (a CMake function) and `add_subdirectory`s each component; the shim
  folds the component's `SRCS`/`INCLUDE_DIRS` straight into the `simulator`
  executable. `REQUIRES` are ignored (one binary → includes are global; IDF-only
  requirements like `esp_lvgl_port` have no host counterpart). Both the shared
  components and the **simulator-only `idf_compat` component** (its own
  `CMakeLists.txt`) are consumed this way — so growing `idf_compat` just means
  adding `SRCS` to that file, not editing `simulator/CMakeLists.txt`. Only
  `simulator/platform/` (the SDL/LVGL entry) stays a direct
  `target_sources` — it's the executable's "main", not a component. A shared
  component that builds differently per target (like `m5stack-bsp`) branches on
  `ESP_PLATFORM` inside its own `CMakeLists.txt`; guard anything that touches
  `${COMPONENT_LIB}` (e.g. `-flto`) in the device branch, since the shim doesn't
  define it.

### Where a device/simulator-divergent API goes — the rule

When an API needs different device vs simulator implementations, decide by one
question: **does Espressif already define this API?**

- **Yes** (e.g. `esp_err`, `esp_log`, `nvs_flash`, FreeRTOS, `esp_timer`,
  `esp_heap_caps`) → implement the *ESP-IDF API itself* on the host in
  `simulator/idf_compat/`. Don't re-abstract something already abstracted, and
  app code stays standard ESP-IDF. One canonical home: never let an ESP-IDF compat
  shim grow inside the BSP or another component. (PSRAM allocation is
  `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` — an ESP-IDF API, so app code calls
  it directly on both targets.)
- **No** — it's this board's own hardware concern with no standard contract
  (framebuffer, touch point, brightness) → put it behind the **BSP** (`bsp_*`),
  which already has both a device and a simulator implementation. App code calls
  `bsp_*` on both targets; the per-target split lives inside the BSP (device chip
  driver vs SDL backend), selected per board. Keep the `bsp_*` surface small and
  demand-driven; don't pre-build a speculative one.

Ambiguous-looking cases resolve cleanly under this rule: `nvs_flash` is an
ESP-IDF API → it's `idf_compat` and app code calls the C API directly (see the
NVS section), **not** a BSP seam.

### The `m5stack-bsp` component (shared board support — the platform seam)

`bsp_*` **is** the cross-platform hardware seam: `app/` calls `bsp_*` directly on
both targets (there is no separate `pf_port` layer). The component is shared
(`components/m5stack-bsp/`) and builds device drivers or the SDL simulator backend
from its own `ESP_PLATFORM`-branched `CMakeLists.txt`.

Structured so non-Tab5 M5Stack models can be added later without reworking the
drivers. Three layers:

- **Public API (`inc/bsp.h`)** — model-agnostic: `bsp_init(const bsp_config_t*)`,
  `bsp_restart()`, `bsp_display_*`, `bsp_touch_*` (touch points are the BSP's own
  `bsp_touch_point_t`, defined in `bsp_types.h` — no `esp_lcd_touch` type leaks
  into the public API). The model-agnostic `bsp_display_*`/`bsp_touch_*`
  functions are implemented **once** in the shared layer (`src/bsp_display.c`,
  `src/bsp_touch.c`): they hold the active provider and dispatch through its
  vtable, so a board never re-implements this glue. `bsp_init()` and
  `bsp_restart()` are the only per-model pieces, under `boards/<model>/` (build
  selects one board). Tab5's `boards/tab5/tab5.c` brings up the buses, resolves
  the panel generation (ST7123 vs ILI9881C/GT911) by **I2C probe + plain `if`**
  (board-internal, not abstracted), creates the matching providers, and
  registers them with `bsp_display_set_active()` / `bsp_touch_set_active()`.
- **Internal driver interfaces (`inc_private/bsp_display.h`, `bsp_touch.h`)** —
  struct-inheritance vtables (esp_lcd style): a driver embeds `bsp_display_t` /
  `bsp_touch_t` as its **first** struct member and returns `&state->base` from a
  `*_create()` factory; the board calls through the members directly
  (`disp->flush(disp, i)` — single indirection). The portable base op is
  `draw_bitmap`; **host-side framebuffers (`get_framebuffers`+`flush`) and
  backlight (`set_brightness`) are optional** — a driver leaves the pointer NULL
  when the panel lacks the capability, so EPD / SPI-with-GRAM panels fit without
  the MIPI framebuffer-swap model baked into the contract. (Today only the
  framebuffer path is wired; the app assumes it.)
- **Device drivers (`devices/`)** — reusable chip drivers, each a `bsp_display`/
  `bsp_touch` provider. They include only `bsp_display.h`/`bsp_touch.h`
  (+`bsp_types.h`), not `bsp_private.h`. Device-only (need `driver/i2c`, `gpio`, …).
- **Simulator backend (`simulator/`)** — the host-side analogue of `devices/`: a
  reusable SDL backend (`sdl_backend.cpp`) that turns one SDL window into a
  `bsp_display` + `bsp_touch` provider. Per-model differences (window title, panel
  geometry, pixel format, frame-buffer count, on-screen scale) are passed via
  `sdl_backend_config_t`, so every model's simulator board shares the same SDL
  plumbing. SDL events are pumped inside `bsp_touch_read` (mouse → touch);
  `set_brightness` is a no-op. SDL2 is linked to the `simulator` executable by
  `simulator/CMakeLists.txt`, so the backend just `#include <SDL2/SDL.h>`.
- **Boards (`boards/<model>/`)** — `bsp_init()`/`bsp_restart()`, the only per-model
  pieces, with a device variant (`<model>.c`) and a simulator variant
  (`<model>_sim.c`); the `ESP_PLATFORM` branch in the component `CMakeLists.txt`
  selects one. Tab5's `tab5.c` probes the panel generation and wires the device
  providers; `tab5_sim.c` hands Tab5's geometry to `sdl_backend_create()` and
  registers the SDL providers.

Everything in the BSP is **C** (both targets, like the device drivers). In the
vtable header `inc_private/bsp_touch.h`, `bsp_touch_config_t` (device bus/GPIO
wiring) is `#ifdef ESP_PLATFORM` so the host build never pulls in `driver/*`.

Audio (`bsp_tab5_audio_*` in `inc/bsp_audio.h`) is Tab5-specific with no
cross-model contract yet, so it keeps its name and lives in the tab5 board.
`audio_eq.c` is device-only for now (the host has no audio path); it moves into
the shared sources once a simulator audio board exists.

### App entry & the LVGL runtime (not in the BSP)

`bsp_*` covers hardware; the LVGL **runtime** (the task/loop that drives
`lv_timer_handler`) is a per-target runtime concern and stays out of the BSP:

- Device: `esp32p4/main/main.cpp` `app_main()` starts esp_lvgl_port
  (`lvgl_port_init`) then calls `adb_app()`.
- Simulator: `simulator/platform/main.cpp` `main()` runs `lv_init`, sets the LVGL
  tick/delay to SDL, then calls `adb_app()` and runs the `lv_timer_handler` loop
  on the main thread. No scheduler bootstrap — host FreeRTOS tasks are pthreads
  (see "FreeRTOS on the host" below), so `main()` mirrors device `app_main()`.

`adb_app()` in `app/adb_app.cpp` is the shared entry: it calls `bsp_init()`, sets
up the LVGL display on the two `bsp_display` frame buffers + an indev on
`bsp_touch_read`, and pushes the first screen. Panel is 720×1280 portrait RGB565
(`PANEL_W`/`PANEL_H` in `app/adb_app.hpp`).

### NVS — the `nvs_flash` C API, used directly

NVS is the worked example of the `idf_compat` rule. The seam is the ESP-IDF
`nvs.h`/`nvs_flash.h` **C API** itself, so shared code calls that C API directly
on both targets.

- Device: the real flash-backed `nvs_flash` component (whatever needs it lists
  `REQUIRES nvs_flash`).
- Simulator: the JSON-backed compat impl in `simulator/idf_compat/`
  (`nvs.h` + `nvs_flash.h` + `nvs.c`). The JSON file defaults to `nvs_data.json`
  in the cwd; override with the sim-only `nvs_flash_sim_set_path()` before the
  first open.

Keep NVS as the C API on both sides. A C++ convenience layer, if wanted, belongs
in a shared component on top of the C API — not as a per-target file.

### The `embedded_adb` component (ADB host-side client — work in progress)

Tab5 plays the **ADB host** (like WebADB / ya-webadb): it drives a USB-connected
Android device. Only the host side of the protocol is implemented. Modeled on the
upstream ADB sources (read-only reference). C++ so the
async stream/auth logic isn't a C callback maze.

Layering (one concern per pair, all portable C++ **except the transport**):
- `adb_protocol` — pure wire format: `MessageHeader` (24-byte `amessage`),
  `Packet` (`apacket`), command/auth constants, checksum. No I/O.
- `adb_crypto` — RSA-2048 keygen, token signing (PKCS#1 v1.5 + SHA1), and the
  Android public-key blob. Uses **mbedTLS on both targets** (ESP-IDF bundles it;
  the simulator gets it from Nix) — crypto is a third-party lib, not an ESP API
  or a board concern, so it is *not* re-abstracted and needs no `idf_compat` shim.
- `adb_keystore` — persists the RSA private key via the **NVS C API** (per the NVS
  rule above; JSON-backed in the simulator). We **never read the host's
  `~/.android/adbkey`** — always generate/store our own key in NVS.
- `transport` — USB bulk transfer to the ADB interface (USB class `0xFF` /
  subclass `0x42` / protocol `0x01`). This is the **only device/simulator split**:
  `transport_usbhost.cpp` (esp-idf `usb_host`) vs `transport_libusb.cpp` (libusb),
  selected by the `ESP_PLATFORM` branch in the component `CMakeLists.txt` — same
  pattern as `m5stack-bsp`. It lives inside `embedded_adb`, not the BSP: the
  `usb_host` API is too large to reimplement on the host and the need (find iface,
  open bulk IN/OUT eps, transfer) is ADB-specific, not a generic board seam.
- `adb_connection` / `adb_stream` — CNXN handshake + AUTH state machine, and
  `A_OPEN/OKAY/WRTE/CLSE` stream multiplexing (`open_stream()` + `run_service()`
  for one-shot commands, classic per-OKAY flow control). The packet read loop is
  `run_blocking()` — the *caller* owns the thread (a `std::thread` in the host
  tests, a FreeRTOS task in the app); the library only relies on a thread-safe
  `send()` + stream registry, so it stays thread-model-agnostic. (`start()` /
  `adb_client` high-level API land with P7.)

Dev strategy: the simulator's **libusb transport talks to a real Android device
plugged into the PC**, so the protocol/auth/stream layers are developed and
debugged on the desktop against a real phone; only `transport_usbhost.cpp` needs
on-device validation. (Run `adb kill-server` first so the host adb-server doesn't
hold the interface.) SDL2/libusb/mbedTLS are linked to the `simulator` exe in
`simulator/CMakeLists.txt`; `flake.nix` provides `libusb1` + `mbedtls` for the host.

`components/embedded_adb/test/` has two host harnesses (build commands in each
file's header comment):
- `test_crypto.cpp` — no phone needed: DER round trip, token signature verify, and
  the Android public-key blob invariants (modulus/exponent/n0inv/rr).
- `test_connect.cpp` — **needs a phone** (USB debugging on, `adb kill-server`
  first): runs the real CNXN+AUTH handshake over libusb. Compile the idf_compat
  `.c` deps (`nvs.c`, `esp_err.c`) with `gcc` (not `g++` — they use C void* casts)
  into `.o`, then link with the C++ sources + `libcjson`/`mbedtls`/`libusb-1.0`.
  Verified end-to-end against a real Android device: first run prompts "Allow USB debugging?",
  the key persists to NVS, and the second run reconnects with no prompt.
- `test_shell.cpp` — needs a phone (already authorized): runs the read loop on a
  `std::thread` and executes `shell:` commands via `run_service()`. Verified on a
  real Android device (`getprop`/`echo`/`id` return correct output). The read loop is started
  with `std::thread` here; the app (P7) wraps `run_blocking()` in a FreeRTOS task.

## Simulator details

### FreeRTOS on the host — a pthread-backed API compat (not the real kernel)

The simulator does **not** run the FreeRTOS kernel. Instead the FreeRTOS *API
contract* is reimplemented on native pthreads in `simulator/idf_compat/`
(`include/freertos/*.h` + `src/freertos_*.c`) — the same "reimplement the
contract, don't port the implementation" philosophy as the `esp_*` shims. A task
**is** a detached pthread scheduled by the host OS; there is no scheduler to
start, no tick ISR, no POSIX-signal machinery.

This is deliberate: the previous vendored FreeRTOS-Kernel GCC/Posix port
emulated a single-core, signal-driven scheduler on pthreads, which forced a pile
of fragile constraints (SDL had to own the main thread *and* the scheduler had to
run on a background thread because `vTaskStartScheduler()` blocks forever; tasks
could only be spawned post-boot or the port's `pthread_once` signal setup
corrupted; two macOS/arm64 port bugs). The host OS already has a scheduler, so
running a second one bought scheduling fidelity this UI/app-logic simulator does
not need. The compat layer deletes all of that.

What this means for app code:
- **`xTaskCreate()` works anywhere, anytime** — including directly from
  `adb_app()` / `bsp_init()`. No "spawn only post-boot", no `lv_async_call`
  dance for non-LVGL FreeRTOS work. The device and simulator entry points are now
  symmetric: `app_main()` (device) and `main()` (sim) both just call `adb_app()`.
- The **one** semantic gap: real hardware runs one task at a time per core, so a
  critical section is atomic against all other tasks. With pthreads tasks run
  truly in parallel, so `taskENTER_CRITICAL`/`portENTER_CRITICAL` (incl. the
  ESP-IDF `portMUX_TYPE*` form) map to a single global recursive mutex — the "big
  kernel lock" — which restores single-at-a-time semantics *inside* critical
  sections but does not serialize ordinary task code. Priorities are stored but
  not enforced; `*FromISR` variants forward to their blocking counterparts
  (no ISRs on the host); `portYIELD_FROM_ISR` is a no-op.
- Tick rate matches the device (`configTICK_RATE_HZ = 100`) so `pdMS_TO_TICKS()`
  and raw tick delays behave the same on both targets.

Still true: **SDL/LVGL own the main thread** (`SDL_PollEvent`/Cocoa on macOS), so
`simulator/platform/main.cpp` runs the `lv_timer_handler` loop on the main thread.
But that is now the *only* main-thread rule — it no longer interacts with task
creation. LVGL itself is single-threaded on both targets, so a FreeRTOS task that
needs to touch LVGL still marshals via `lv_async_call` (device: `lvgl_port_lock`),
exactly as on hardware.

### Other simulator notes
- LVGL config is `simulator/lv_conf.h` (found via `LV_CONF_INCLUDE_SIMPLE` +
  the project source dir). `LV_COLOR_DEPTH 16` (RGB565, matches the panel) and
  `LV_USE_THEME_DEFAULT 1` (the e-paper-derived base config had it off).
- Host compat for shared code lives in the `simulator/idf_compat/` component.
  **`simulator/idf_compat/README.md` is the source of truth** (layout, the
  include-path rules, how to add a shim, the FreeRTOS compat surface). In short:
  `include/` = shim headers, `src/` = shim sources. Current shim surface:
  - `esp_err` — `esp_err_t`, `ESP_ERR_*`, `esp_err_to_name()`, `ESP_ERROR_CHECK[_WITHOUT_ABORT]`.
  - `esp_log` — `ESP_LOGx` to stderr, level enum, `esp_log_level_set` (no-op).
  - `esp_check` — `ESP_RETURN_ON_ERROR` / `ESP_GOTO_ON_*` / `ESP_RETURN_ON_FALSE`.
  - `esp_timer` — `esp_timer_get_time()` (monotonic µs) only.
  - `esp_heap_caps` — `heap_caps_malloc`/`calloc`/`realloc`/`aligned_alloc`/`free`
    (host malloc; `MALLOC_CAP_*` flags accepted and ignored).
  - `nvs`/`nvs_flash` — JSON-backed NVS C API (see the NVS section).
  - `freertos` — pthread-backed FreeRTOS API: tasks, delays, queues, semaphores
    (binary/counting/mutex/recursive), event groups, task notifications, software
    timers, critical sections (`include/freertos/*.h` + `src/freertos_*.c`).
  `INCLUDE_DIRS` is just `include`; `freertos/FreeRTOS.h` resolves because
  `include` is on the path. Add a shim: header in `include/`, source in `src/` +
  its `SRCS` entry — nothing else to wire. Grow the FreeRTOS surface the same way
  (add to an existing `freertos_*.c` or a new one). pthreads link to the
  `simulator` exe via `Threads::Threads` in `simulator/CMakeLists.txt`.
- Build artifacts (`build/`, `simulator/.deps/`) are gitignored. LVGL is fetched
  into `.deps`.
