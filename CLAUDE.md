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

### Android companion — `android-agent/` (`tab5adb-agent`)

The program side-loaded onto the Android phone — **screen mirroring** (main
purpose) plus offload of work the Tab5 can't do; mirroring is one service among
several. It is a **scrcpy-style server, not an APK**: a plain **Java** program
dexed into a jar, pushed to `/data/local/tmp`, and launched with `app_process` so
it runs with shell uid (2000) and reaches hidden Android APIs (display capture,
input injection) with no permission dialog. It listens on the abstract socket
`localabstract:tab5adb-agent`; the Tab5 host reaches it over its embedded ADB.

The flake gained the Android toolchain for this (`pkgs.jdk`, `pkgs.android-tools`
for a standalone `adb`, and `androidenv.composeAndroidPackages` for `android.jar`
+ `d8`; `allowUnfree` / `android_sdk.accept_license` are set for the SDK). The
shellHook exports `ANDROID_JAR` and puts `d8` on `PATH`.

```sh
nix develop -c android-agent/build.sh      # javac + d8 -> build/tab5adb-agent.jar
nix develop -c android-agent/run.sh        # adb push + app_process (foreground)
# from another shell, reach the socket from the PC:
nix develop -c adb forward tcp:8080 localabstract:tab5adb-agent
```

Dev strategy mirrors `embedded_adb`'s: the agent is developed and verified
against a **real phone plugged into the PC with standard adb** (push + `app_process`
+ `adb forward localabstract:`), so the Tab5 wiring isn't on the critical path.
Only once the agent works does the Tab5 side get built: the firmware will push the
embedded dex over the existing `sync:` service, launch it via `shell:`/`exec`, and
connect via a `localabstract:` stream open. That open is **not** a missing engine
piece — `embedded_adb`'s `AdbConnection::open_stream(service, …)` is generic, so
`open_stream("localabstract:tab5adb-agent", …)` already works; what the Tab5 side
needed was a generic `adb`-layer stream + the agent-protocol logic, both now built
(see `agent_link` below). The dex is tiny (~3.5 KB), so the plan is to embed it
gzip+`xxd` → C array, falling back to a dedicated partition if it grows.
**Phase 1 done** (build + `app_process` launch + socket banner). **Wire protocol
specified** in `android-agent/docs/protocol.md` (single-socket TYPE multiplexing,
no payload CRC, agent-initiated HELLO over a forward `localabstract:` connection,
Android→Tab5 JPEG strip stream — YUV420 q60, agent-side rotate/scale (fit/fill)/
strip, 16px aligned; audio reserved). **HELLO handshake done & verified on a
real Android device** (agent sends HELLO REQUEST + checks the Tab5 RESPONSE's proto match;
Tab5-side `agent_link` answers): the headless harness
`components/agent_link/test/test_hello.cpp` drives the whole bring-up GUI-less over
libusb (connect → `Sync::push` the jar → `open_shell` to launch `app_process` →
`Link::open` localabstract → HELLO round-trip), both sides logging `HELLO ok`. Run
it with `nix develop -c components/agent_link/test/run.sh`; the test approach is
documented in `android-agent/docs/testing.md`. Next: Phase 2 (JPEG strip
capture/receive). See `android-agent/README.md`.

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
  adb/                       #   app-facing object API over embedded_adb (Client/Shell/Sync/Stream)
    inc/                     #     adb.hpp umbrella + adb_client.hpp, adb_raw_stream.hpp, adb_error.hpp
    src/  test/              #     impl + host test
    README.md  docs/         #     README = front door; docs/<surface>.md = per-surface spec
  agent_link/                #   Tab5-side link to tab5adb-agent (protocol.md) — over adb::Stream
    inc/                     #     agent_link.hpp (Link/LinkListener) + agent_link_protocol.hpp (wire)
    src/  test/              #     impl + host HELLO test (test_hello.cpp)
esp32p4/                     # DEVICE build root (IDF project)
  main/                      #   entry point (app_main + device LVGL runtime via esp_lvgl_port)
simulator/                   # SIMULATOR build root (see below)
  platform/                  #   SIM-only entry (main.cpp: SDL/LVGL timer loop)
  idf_compat/                #   SIM-only ESP-IDF compat component (see its README.md)
    include/  src/           #     host shims: esp_* (err/log/check/timer/heap/nvs)
                             #     + freertos/* (pthread-backed FreeRTOS API)
android-agent/               # ANDROID  tab5adb-agent (scrcpy-style app_process server)
  src/                       #   Java sources (com.tab5adb.agent.Server)
  build.sh  run.sh           #   javac+d8 -> dex jar; adb push + app_process dev loop
```

`m5stack-bsp` is the worked example of a target-divergent shared component: its
one `CMakeLists.txt` branches on `ESP_PLATFORM` (set only under ESP-IDF) to build
the device drivers + tab5 board on device and the SDL backend + tab5 sim board on
the host.

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
  open bulk IN/OUT eps, transfer) is ADB-specific, not a generic board seam. The
  esp-idf usb_host API is async (transfers complete via callbacks pumped by two
  background tasks — `usb_host_lib_handle_events` + `usb_host_client_handle_events`);
  `transport_usbhost.cpp` wraps it as the synchronous `Transport` with a binary
  semaphore per direction. **Critical gotcha (cost a debugging session):** the
  default USB host FIFO bias (`BALANCED`) gives the non-periodic TX FIFO only
  `dfifo_depth/16` lines → a **bulk OUT MPS limit of 256**, so claiming an Android
  phone's 512-byte high-speed bulk endpoints fails with `interface_claim →
  ESP_ERR_NOT_SUPPORTED`. ADB is bulk-only, so we set `usb_host_config_t`'s
  `fifo_settings_custom` to rx=256 / nptx=256 / ptx=0 lines (MPS limits ~1016 /
  1024; P4 DFIFO is 1024 lines, sum must be ≤ that). On device, advertise a modest
  CNXN maxdata (16 KB) so the usb_host transport's per-payload DMA allocations stay
  small.
- `adb_connection` / `adb_stream` — CNXN handshake + AUTH state machine, and
  `A_OPEN/OKAY/WRTE/CLSE` stream multiplexing (`open_stream()` + `run_service()`
  for one-shot commands, classic per-OKAY flow control). The packet read loop is
  `run_blocking()` — the *caller* owns the thread (a `std::thread` in the host
  tests, a FreeRTOS task in the app); the library only relies on a thread-safe
  `send()` + stream registry, so it stays thread-model-agnostic. On teardown
  (`run_blocking()` returns) it closes any still-open streams so every owner gets
  a terminal `on_close` (one-shot completions fire exactly once);
  `AdbStream::mark_closed()` is idempotent so the peer's `A_CLSE` and teardown
  can't double-fire. (`start()` / `adb_client` high-level API land with P7.)

Dev strategy: the simulator's **libusb transport talks to a real Android device
plugged into the PC**, so the protocol/auth/stream layers are developed and
debugged on the desktop against a real phone; only `transport_usbhost.cpp` needs
on-device validation. (Run `adb kill-server` first so the host adb-server doesn't
hold the interface.) SDL2/libusb/mbedTLS are linked to the `simulator` exe in
`simulator/CMakeLists.txt`; `flake.nix` provides `libusb1` + `mbedtls` for the host.

`components/embedded_adb/test/` has three host harnesses, run via the component's
**test runner** `nix develop -c components/embedded_adb/test/run.sh` (`TEST=<name>`
selects `test/<name>.cpp`; the runner compiles the idf_compat `.c` deps with `gcc`
— not `g++`, they use C void* casts — links everything, and runs `adb kill-server`
for the device tests; artifacts in `test/build/`):
- `test_crypto.cpp` (the default; **no phone**): DER round trip, token signature
  verify, and the Android public-key blob invariants (modulus/exponent/n0inv/rr).
- `test_connect.cpp` — **needs a phone** (USB debugging on): runs the real
  CNXN+AUTH handshake over libusb. Verified end-to-end against a real Android device: first run
  prompts "Allow USB debugging?", the key persists to NVS, and the second run
  reconnects with no prompt.
- `test_shell.cpp` — needs a phone (already authorized): runs the read loop on a
  `std::thread` and executes `shell:` commands via `run_service()`. Verified on a
  real Android device (`getprop`/`echo`/`id` return correct output). The read loop is started
  with `std::thread` here; the app (P7) wraps `run_blocking()` in a FreeRTOS task.

On hardware: phone goes on the **Tab5's USB-A host port** (not the Mac); flash
over USB-C. `idf.py monitor` needs a TTY, so capture the boot log by resetting via
RTS and reading the port with a short PySerial script (the P4 prints logs only
once after reset). The full ADB host stack was verified end-to-end on the real
Tab5 + an Android phone (claim → auth → `shell:` output).

### The `adb` component (app-facing object API — in progress)

The layer the app actually drives, on top of `embedded_adb`. Where
`embedded_adb` is the thread-agnostic protocol engine, **`adb` owns the
connection lifecycle** (RSA key, USB transport, the reader task) and exposes a
typed, object-oriented surface. It's a **separate component** (not folded into
`embedded_adb`) so the engine keeps its `std::thread`-only host unit tests while
`adb` is free to use the FreeRTOS API (`xTaskCreate`) for the reader task — works
on both targets (real kernel on device, pthread compat in the sim). It shares
`namespace adb`; high-level names (`Client`/`Shell`/`Sync`) don't collide with
the engine's (`AdbConnection`/`AdbStream`/`Packet`). No source split — all
portable C++ over `embedded_adb` + FreeRTOS, one `idf_component_register`.

**Docs: `components/adb/README.md` is the front door** (overview + the
cross-cutting rules every API obeys + roadmap); **per-surface detail lives in
`components/adb/docs/<surface>.md`** (e.g. `docs/client.md`), one file per API
surface, grown slice by slice. **Write/update the relevant doc before
implementing.** The agreed design (read the docs for the full contract):
- **Archetypes:** *sessions* (long-lived: `Shell`, `Sync`) use an abstract-class
  listener with the originating object as the **first callback arg** (one
  listener serves multiple objects); *one-shots* (`screencap`, `exec`, each
  `Sync` op) use a `std::function` completion (the closure is the correlation, no
  tag). Filesystem = the `sync:` stream is itself a session whose *methods* are
  one-shot-style.
- **Threading:** all callbacks fire on the reader thread; **marshalling to LVGL
  is the app's job** (the library never touches LVGL). Methods are non-blocking
  and callable from any thread (e.g. `Shell::write()` enqueues, returns
  `adb::Error`; backpressure → `Error::QueueFull`).
- **Lifetime:** sessions are `shared_ptr`; every terminal callback (`on_*_close`
  / one-shot completion) fires **exactly once** (incl. `Error::Cancelled` from
  `Client::close()`). The library holds the **listener as a `weak_ptr`** and
  `lock()`s it before each dispatch, so there is **no `detach()`**: `close()`
  stops I/O, and simply dropping the listener's `shared_ptr` detaches (an expired
  `lock()` skips the callback racelessly). A session's `on_*_close` is delivered
  through the weak listener, so it is skipped if the listener was already dropped.

Built incrementally in commit-sized slices (see the roadmap in the component
`README.md`). **Slice 1** (done): `Client::connect_usb()` + `state()`/`banner()` +
`close()`; verified by
`test/test_client.cpp` (libusb vs a real phone, same harness pattern as
`embedded_adb/test`). `Client` runs the read loop on an internal FreeRTOS task and
joins it in `close()`/dtor; this relies on `AdbConnection::stop()` being honored
even **before** `run_blocking()` starts (a `stop_requested_` gate added for
race-free teardown). **Slice 2** (done): the `exec` one-shot
(`Client::exec(cmd, cb)` → collects `shell:<cmd>` output, completion on the reader
thread, see `docs/one-shots.md`) plus wiring the UI onto `Client` (the app-global
holder lives in `adb_app`; getprops go through `exec`, so `adb` doesn't leak an
`AdbConnection`). To make a one-shot completion fire **exactly once**,
`AdbConnection::run_blocking()` teardown closes outstanding streams (and
`AdbStream::mark_closed()` is idempotent).
**Slice 3** (done): the `Shell` session — `Client::open_shell(listener, cmd="")`
returns a `shared_ptr<Shell>` (an interactive PTY `shell:` when `cmd` is empty,
`shell:<cmd>` otherwise); `ShellListener` delivers `on_shell_data`/`on_shell_close`
(exactly once) on the reader thread, and non-blocking `Shell::write()` enqueues to
a **per-Shell writer task** that owns the blocking `AdbStream::write()` (one
`A_WRTE` per `A_OKAY`) — so the engine stays thread-agnostic while the `adb` layer
spends a FreeRTOS task on it. Backpressure → `Error::QueueFull` past a ~64 KB
per-stream cap; the listener is held as a `weak_ptr` (`lock()`ed before each
dispatch), so dropping the listener's `shared_ptr` detaches. See
`docs/shell.md`;
verified by `test/test_shell.cpp` (libusb vs a real phone). **Slice 5** (done):
the `Sync` session for the FileManager — `Client::open_sync(listener)`
returns a `shared_ptr<Sync>` over the `sync:` service; built **direction by
direction** (each is its own UI), starting with **Tab5→Android `push` (SEND)**
plus `stat` (STAT) as its verifier, then `list` (LIST) for the browse UI. The
`sync:` sub-protocol is request/response and **serial** (one
request at a time on the one stream), so unlike `Shell`, `Sync` owns a private
**worker task** that drives it synchronously: it writes requests and *blocks for
responses* over an internal **byte pipe** (`std::condition_variable`) fed by the
reader thread's `on_data`. **Threading refinement (documented divergence from the
"callbacks fire on the reader thread" rule):** `Sync` op completions and
`on_sync_close` fire on this **worker thread**, never the reader thread — still
never the LVGL thread, so the app marshals as usual. Two wire gotchas baked into
the impl: (1) `AdbConnection::send_write()` does **not** split, so each sync
`DATA` `A_WRTE` is capped to the negotiated `max_payload` (16 KB on device) — a
64 KB sync chunk is chopped to fit; (2) SEND v1 sends the request path as
`"<path>,<st_mode decimal>"` (mode = `perm | S_IFREG`) and terminates with a
`DONE` whose length field carries the mtime. Sync sub-protocol ids are pure wire
format in `embedded_adb/inc/adb_sync_protocol.hpp` (alongside `adb_protocol.hpp`,
no I/O). See `docs/sync.md`; verified by `test/test_sync.cpp` (libusb vs a real
phone: push a buffer → stat it back). Later slices add `screencap`.

`adb` also exposes a **generic, service-agnostic raw stream** —
`Client::open_stream(service, StreamListener) -> shared_ptr<adb::Stream>`
(`adb_raw_stream.hpp`), the untyped building block beside `Shell`/`Sync`. It is
essentially `Shell` minus the `shell:` PTY semantics: a non-blocking `write()`
backed by a per-stream writer task, `on_stream_data`/`on_stream_close` on the
reader thread, listener held weakly. Its reason to exist is **dependency
direction**: app-specific protocols in *other* components (e.g. `agent_link`)
build on this so they depend on `adb`, never on `embedded_adb` directly.

### The `agent_link` component (Tab5-side link to tab5adb-agent)

The Tab5 end of the `tab5adb-agent` wire protocol
(`android-agent/docs/protocol.md`): one TYPE-multiplexed ADB stream carrying
control + video + (future) audio. **Dependency arrow is `agent_link` (App-specific)
→ `adb` (generic) → `embedded_adb`**; the generic `adb::Client` never knows about
`agent_link` (no `open_agent_link()` on `Client` — that would invert it). The
entry point lives here: `agent_link::Link::open(shared_ptr<adb::Client>, listener,
cfg)` calls `client->open_stream("localabstract:tab5adb-agent", …)` and layers
framing on top. `Link` **is** an `adb::StreamListener` (passed to the stream as a
`weak_ptr` — drop the `Link`'s `shared_ptr` to detach, Shell/Sync-style). Pure
wire format (frame header §3, TYPE/FLAGS, HELLO §4, status codes) is in
`inc/agent_link_protocol.hpp` (no I/O, the `agent_link` analogue of
`adb_protocol.hpp`). Unlike `Sync`'s worker+pipe, the parser is **reactive on the
reader thread**: `agent_link` is mostly a receiver (agent→Tab5 push), so
`on_stream_data` accumulates bytes and dispatches whole frames; the HELLO response
`write()` is non-blocking. So `on_link_hello`/`on_link_close` fire on the **reader
thread** (LVGL marshalling is the app's job; the headless test needs none). This
same parser carries the JPEG strip stream in Phase 2 by handing each `FRAME_END`
to a decode+framebuffer seam (host libjpeg in the test, P4 HW JPEG + bsp FB in the
app). **HELLO done & verified on a real Android device** by `test/test_hello.cpp` (run it with
`nix develop -c components/agent_link/test/run.sh` — the runner builds the whole
host stack, runs `adb kill-server`, and launches the test; artifacts in
`test/build/`; test approach in `android-agent/docs/testing.md`). Next slices:
JPEG strip receive (Phase 2).

### The provisional UI (HomeScreen → ADBDeviceScreen → ADBShellScreen)

`app/` drives the connection from the LVGL UI: `HomeScreen` has a **Connect**
button; tapping it calls `app::adb_connect_async()` — a small app-global holder in
`app/adb_app.cpp` that owns the single `std::shared_ptr<adb::Client>` (it must
outlive the transient screens) and implements `adb::ClientListener`. The `Client`
owns the connection lifecycle + reader task; the holder's only job is to marshal
the reader-thread `on_state` callbacks to the LVGL thread with `lv_async_call`
(marshalling is the app's job — the library never touches LVGL). On reaching
Online it pushes `ADBDeviceScreen`, which shows banner-derived fields
(model/name/device) and fetches a few live `getprop`s via a single
`app::adb_client()->exec(...)` one-shot (its completion fires on the reader thread,
so the label update is marshalled back to LVGL). The app pulls in no
protocol/transport details, only the `adb` component's typed surface.

`ADBDeviceScreen` has an **Open Terminal** button that pushes **`ADBShellScreen`**
(`app/adb_shell_screen.*`) — an interactive terminal over `Client::open_shell()`.
The screen **is** the `adb::ShellListener`: device output streams into a read-only
monospace (`lv_font_unscii_16`) `lv_textarea`, and an input `lv_textarea` + an
on-screen `lv_keyboard` send a line per OK keypress (`shell_->write(line+"\n")`;
the PTY echoes input back, so the UI does **not** local-echo). Two threading
concerns the screen handles, both from the cross-cutting `adb` contract: (1) Shell
callbacks fire on the **reader thread**, so `on_shell_data`/`on_shell_close`
sanitize (strip CR + ANSI/VT escapes the bitmap font can't render) and marshal the
widget update with `lv_async_call`; (2) the screen (the listener) can be destroyed
on `pop()`, so `onExit()` just runs `shell_->close()` (the shell holds the
listener as a `weak_ptr` — the screen passes a `shared_ptr` aliasing
`shared_from_this()`, and the weak ref expires when the screen frees).
Each marshalled lambda captures `self = shared_from_this()` (keeping the screen
alive until it drains on the LVGL thread) and skips when the base
`Screen::exited()` flag is set, so an update already queued before teardown skips
the freed widgets — race-free because teardown and the `lv_async_call` body both run
on the LVGL thread. (`ScreenManager` owns screens via `shared_ptr` and sets
`exited_` right before `onExit()` — see the screen_manager component.)
`LV_FONT_UNSCII_16` is enabled
on both targets for the terminal (sim `lv_conf.h`; device `sdkconfig`/
`sdkconfig.defaults`). v1 is line-oriented (touch keyboard), not a raw VT.

`ADBDeviceScreen`'s **File Manager** button pushes **`ADBFileManagerScreen`**
(`app/adb_file_manager_screen.*`) — a virtual root that lists the available
storages (Android `/sdcard`, `/`, and the Tab5 SD card) as cards. Tapping a
storage pushes **`ADBFileBrowserScreen`** (`app/adb_file_browser_screen.*`), the
read-only Android file browser over `Client::open_sync()` + `Sync::list()`. The
browser **is** the `adb::SyncListener`: it opens a `sync:` session, lists the
directory (folders first, then case-insensitive by name; `.`/`..` filtered) and
tapping a folder descends into it. A `directory_stack_` holds the path history —
each level caches its entries — and the nav **Back** button goes up a level, or
pops the screen at the stack root. Same two threading concerns as the shell
screen, but note the Sync refinement: `list()` completions fire on the **Sync
worker thread** (not the reader thread), so they marshal to LVGL with
`lv_async_call`; `onExit()` just does `close()` (the session holds the listener
as a `weak_ptr` — the screen passes a `shared_ptr` aliasing `shared_from_this()`,
so no `detach()`); marshalled lambdas capture `self = shared_from_this()` and
skip on `Screen::exited()`. One extra guard: a `nav_gen_` counter (LVGL-thread
only) bumped on every navigation drops **stale list completions** when the user
taps faster than the device responds.

## Simulator details

### FreeRTOS on the host — a pthread-backed API compat (not the real kernel)

The simulator does **not** run the FreeRTOS kernel. Instead the FreeRTOS *API
contract* is reimplemented on native pthreads in `simulator/idf_compat/`
(`include/freertos/*.h` + `src/freertos_*.c`) — the same "reimplement the
contract, don't port the implementation" philosophy as the `esp_*` shims. A task
**is** a detached pthread scheduled by the host OS; there is no scheduler to
start, no tick ISR, no POSIX-signal machinery.

This is deliberate: the host OS already has a scheduler, so emulating a
single-core, signal-driven FreeRTOS scheduler on pthreads would buy scheduling
fidelity this UI/app-logic simulator does not need — at the cost of fragile
constraints (SDL must own the main thread *and* the scheduler would have to run
on a background thread because `vTaskStartScheduler()` blocks forever; tasks
spawnable only post-boot or the port's `pthread_once` signal setup corrupts;
macOS/arm64 port bugs). The pthread-backed API contract avoids all of it.

What this means for app code:
- **`xTaskCreate()` works anywhere, anytime** — including directly from
  `adb_app()` / `bsp_init()`. No "spawn only post-boot", no `lv_async_call`
  dance for non-LVGL FreeRTOS work. The device and simulator entry points are
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

**SDL/LVGL own the main thread** (`SDL_PollEvent`/Cocoa on macOS), so
`simulator/platform/main.cpp` runs the `lv_timer_handler` loop on the main thread.
That is the *only* main-thread rule, and it is independent of task creation. LVGL
itself is single-threaded on both targets, so a FreeRTOS task that needs to touch
LVGL marshals via `lv_async_call` (device: `lvgl_port_lock`), the same on both
targets.

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
