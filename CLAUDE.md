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
(see `agent_link` below). The dex is tiny (~9 KB with the Phase 2 pipeline), so
the plan is to embed it gzip+`xxd` → C array, falling back to a dedicated
partition if it grows.
**Phase 1 done** (build + `app_process` launch + socket banner). **Wire protocol
specified** in `android-agent/docs/protocol.md` (single-socket TYPE multiplexing,
no payload CRC; agent-initiated **HELLO = link establishment only** — proto /
version / capability — over a forward `localabstract:` connection; mirror starts
on a separate **Tab5-initiated `MIRROR_START`** carrying panel size / scale mode /
streams; a Tab5-initiated **`MIRROR_STOP`** stops the stream and returns the agent
to READY **without dropping the link** (so a later `MIRROR_START` resumes);
Android→Tab5 JPEG strip stream — YUV420 q60, agent-side rotate/scale
(fit/fill)/strip, 16px aligned; audio reserved as a `MIRROR_START` AUDIO bit).
**Phase 2 (JPEG strip mirror) done & verified on a real Android device.** The Java agent
(`Server` + `FramePipeline`/`Projection`/`TestPattern`/`ScreenCapture`) captures
the screen via hidden display APIs into a fixed 720×1280 `ImageReader` and streams
it as JPEG strips. **Control and video run concurrently** (§4.4): a dedicated
**control reader thread** reads every inbound frame (HELLO response, MIRROR_START,
MIRROR_STOP) while the main thread sends JPEG, so `MIRROR_STOP` is never blocked
behind the video flow; on MIRROR_STOP the session loop stops streaming and goes
back to waiting for the next MIRROR_START (READY) on the same socket. Frame writes
from both threads are serialized in `Conn` (`synchronized writeFrame`). **Geometry (rotate → scale-fit → black letterbox) is
GPU-offloaded for real capture** via one of two creation paths (scrcpy's order),
both landing the final upright/scaled/letterboxed panel-sized frame in the reader
so `acquire()` needs **no CPU readback at source res, no rotate/scale/composite
Bitmap copies**: (1) **primary** = the hidden *static*
`android.hardware.display.DisplayManager.createVirtualDisplay(name, w, h,
displayIdToMirror=0, surface)`, which mirrors display 0 into the panel-sized
surface and lets the compositor do aspect-preserving rotate/scale-fit/letterbox —
**this is the path that works on Android 14/15**, where
`SurfaceControl.createDisplay` was removed (an Android 15 device threw `NoSuchMethodException: SurfaceControl.createDisplay`); (2)
**fallback** (older Android) = `SurfaceControl.createDisplay` +
`setDisplayProjection`, where `ScreenCapture` drives the geometry itself (rotation
code + a centered destination rect computed by the host-testable pure-arithmetic
`Projection`; the area outside it is the virtual display's black background = the
letterbox). Only the fallback honours `scaleMode` (fill vs fit) — the primary
mirror path is always aspect-fit (the mirror default). **Physical-orientation lock
(§5.1 = always show the device's *natural*-orientation framebuffer, ignoring logical
rotation):** the primary mirror follows display 0's *logical* rotation, so turning the
phone would otherwise rotate + shrink-letterbox the Tab5. `ScreenCapture` is built for
the device's current rotation (`Surface.ROTATION_*`) and undoes it: the reader is sized
to the panel *oriented to the rotation* (portrait `targetW×targetH` at 0/180, landscape
`targetH×targetW` at 90/270) so the rotated logical display fills it (GPU scale, no
orientation-mismatch letterbox), then `acquire()` counter-rotates by the inverse rotation
to the natural-orientation `targetW×targetH` frame. A landscape app thus shows sideways +
full-size on the Tab5 (turn the Tab5 to view it), not rotated-upright-and-shrunk. At
ROTATION_0 (common case) the counter-rotation is a no-op = the old GPU-only fast path;
only a rotated device pays one panel-sized `Bitmap` rotation/frame. `Server` polls
`DisplayManagerGlobal.getRealDisplay(0).getRotation()` each frame and rebuilds the capture
when it changes. Natural-lock is primary-path only; the legacy fallback keeps `Projection`'s
source-aspect geometry. (Counter-rotation is `counterDeg = (rotation & 3) * 90`, direction
verified on a real Tab5 + phone.) (GPU geometry is what got
the mirror from a 15fps-capped ~23fps CPU path to ~33-37fps; the old per-frame
full-frame allocs were also GC-thrashing.) The
agent **always emits a full `targetW×targetH` (720×1280) frame**, so every strip
is full panel width (x=0, w=720) — this is what lets the Tab5 decode each strip
straight into its framebuffer row band with no stride (the P4 JPEG 2D-DMA can't
place a narrower picture into a wider buffer; see the JPEG decode seam).
`FramePipeline.stripsOf()` just splits that frame + JPEG-encodes; the
`--test-pattern` path keeps the **CPU** geometry (`FramePipeline.process()`: there
is no SurfaceFlinger to offload to in the headless test), so the GPU-projection
arithmetic is covered instead by the host-JVM `android-agent/test/ProjectionTest`
(`nix develop -c android-agent/test/run.sh`) and its visual result by simverify on
a device. The agent has **no artificial FPS cap** (`Server.TARGET_FPS = 0` =
encoder/capture-rate driven; a static screen yields no new `ImageReader` frame, so
nothing is sent and the Tab5 keeps the last frame). The Tab5-side
`agent_link::Link` parses frames and hands each
strip to a decode+framebuffer seam (`VideoListener::on_video_strip`). The headless
harness `components/agent_link/test/test_mirror.cpp` drives the whole bring-up
GUI-less over libusb (HELLO → `start_mirror` → strips), decodes strips with host
libjpeg, and asserts framing/16-alignment/tiling; the agent's deterministic
`--test-pattern` mode is the primary pass/fail (no `Canvas.drawText` — bare
app_process has no default Typeface), and `TAB5ADB_REAL=1` smoke-tests real
capture. `test_hello.cpp` still covers the HELLO-only path. Run with
`nix develop -c sh -c 'TEST=test_mirror components/agent_link/test/run.sh'`; test
approach in `android-agent/docs/testing.md`. **receive→decode→render done** — the
`ADBMirroringScreen` in `app/` (see the UI section) gets the agent connected via
`app::AgentClient` (which owns the **embedded-jar** push + `app_process` launch +
HELLO), sends `MIRROR_START`, and renders the JPEG strip stream
**directly into the bsp framebuffer** (strided decode + `bsp_display_flush`
triple-buffer, no LVGL compositing), **verified headless in the simulator (libusb)
against a real Android device**: the live home screen mirrors with correct full-range colors
(`./run.sh simverify simulator/verify/mirror.txt`). The agent dex is embedded as a
C array — `app/agent/agent_jar.{h,c}`, `xxd -i` of
`android-agent/build/tab5adb-agent.jar` (regenerate per the header when the agent
changes) — so the push+launch path needs no host file and works on device too.
**Next: real Tab5 E2E** (P4 HW JPEG into the bsp framebuffer over the on-device
USB host). See `android-agent/README.md`.

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
    inc/                     #     agent_link.hpp (Link + LinkLifecycleListener/VideoListener) + agent_link_protocol.hpp (wire)
    src/  test/              #     impl + host HELLO test (test_hello.cpp)
  jpeg_fullrange_decode/     #   full-range BT.601 JPEG decode (mirror strips); ESP_PLATFORM-branched
    include/  src/           #     header + _p4.c (device: CSC-register override) / _sim.c (host delegate)
esp32p4/                     # DEVICE build root (IDF project)
  main/                      #   entry point (app_main + device LVGL runtime via esp_lvgl_port)
simulator/                   # SIMULATOR build root (see below)
  platform/                  #   SIM-only entry (main.cpp: SDL/LVGL timer loop)
  idf_compat/                #   SIM-only ESP-IDF compat component (see its README.md)
    include/  src/           #     host shims: esp_* (err/log/check/timer/heap/nvs)
                             #     + freertos/* (pthread-backed FreeRTOS API)
android-agent/               # ANDROID  tab5adb-agent (scrcpy-style app_process server)
  src/                       #   Java (com.tab5adb.agent): Server + FramePipeline/Projection/TestPattern/ScreenCapture + Input (key + multi-touch injection)
  test/                      #   host-JVM unit test (ProjectionTest) + run.sh — no phone
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
  plumbing. `bsp_touch_read` is now called from a background touch task (see the
  DisplayManager touch section), so SDL — which is main-thread-only on macOS — is
  **not** touched there: the **main thread** drains events + samples the mouse into
  a mutex-guarded touch snapshot in `sdl_backend_pump_input()` (called each
  `main.cpp` loop iteration; the headless harness writes the snapshot directly via
  `inject_down/up`), and `bsp_touch_read` just copies that snapshot.
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

`adb_app()` in `app/adb_app.cpp` is the shared entry: it calls `bsp_init()`, then
`DisplayManager::init()` (which owns the LVGL display on the `bsp_display` frame
buffers, the touch indev, and the mirror overlay compositor — see the
DisplayManager touch section below), and pushes the first screen. Panel is 720×1280 portrait
(`PANEL_W`/`PANEL_H` in `app/adb_app.hpp`); the pixel format is chosen at
`bsp_init` (one line in `adb_app()`) and fixed for the boot — RGB888 by default
(RGB565 also supported). RGB888 framebuffers hold LVGL's native **B,G,R** byte
order; everything that writes a framebuffer agrees on it (the DisplayManager
overlay's PPA 565→888 composite, the mirror's JPEG decode via `BGR` rgb_order,
the sim's `SDL_PIXELFORMAT_BGR24` texture + capture swap). The mirror decode and
the DisplayManager honour `bsp_display_get_pixel_format()`, so the format is the
single boot-time switch.

### DisplayManager touch input (decoupled from LVGL, multi-touch, pushed)

`DisplayManager` (`app/display_manager.{hpp,cpp}`) owns the touch indev, but the
**hardware read is decoupled from the LVGL render loop**: a dedicated FreeRTOS
**touch task** (started in `init()`) does all `bsp_touch_read`, so panel refresh /
JPEG decode latency never delays input. The task is **interrupt-gated + idle-stop**:
it blocks on `bsp_touch_wait_interrupt()` (device = the GT911/ST7123 INT semaphore;
sim = a short `SDL_Delay`), then polls at `touch_poll_hz_` (default **60 Hz**,
`set_touch_poll_hz()` at runtime; quantized to the 100 Hz tick), and after **3
consecutive empty reads** stops polling and waits for the next INT again. This needs
the touch controller INT, so `adb_app()` sets `config.touch.interrupt = true` in the
`bsp_init` config: the GT911/ST7123 INT semaphore is created **only** when that flag
is set, so a caller of `bsp_touch_wait_interrupt()` MUST enable it (otherwise the
driver takes a NULL semaphore and asserts — `bsp_touch_wait_interrupt()` was
previously never called by anyone).

Each sample it (a) updates the **single-tap LVGL feed** (id 0 only — `lvgl_pressed_`
/ `lvgl_pt_`, under `touch_mtx_`) that `indev_read_cb` now just *returns* (no
`bsp_touch_read` on the LVGL thread), and (b) **pushes all contemporaneous points
to a `DisplayManager::TouchListener`** (`on_touch(pts, count)`, count==0 = all
lifted) — the multi-touch, **push not poll** path a feature registers with
`set_touch_listener(weak_ptr)` (held weakly, Shell/Sync-style; **fires on the touch
task thread**, so the listener marshals to LVGL itself). This replaces the old
`touch_point()` poll (removed) + the mirror's `lv_timer`. Each `bsp_touch_point_t`
carries the controller's **pointer track id** (`.id`; GT911/ST7123 forward
`esp_lcd_touch`'s `track_id`, the single-point sim reports 0), so a feature can
correlate fingers across samples without synthesizing ids — the mirror's touch
passthrough keys its per-pointer DOWN/MOVE/UP diff off `.id`. The mirror's
`ADBMirroringScreen` is a `TouchListener`: its `on_touch` runs the corner-swipe
reveal detection and (in touch-control mode) the passthrough diff.

**Swipe-reveal masking (`consume_overlay_touch()`):** when the reveal swipe makes
the hidden overlay visible, the *same* in-flight press would otherwise land as a tap
on a freshly-shown overlay button. `consume_overlay_touch()` sets `lvgl_suppress_`,
which masks the press from the indev until the finger lifts (the task clears it on
the first all-lifted sample); the mirror calls it **before** `set_overlay_visible(true)`
to close the visible-without-mask window.

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
  ESP_ERR_NOT_SUPPORTED`. ADB is bulk-only and for the mirror the **hot direction
  is bulk IN**, so we bias `usb_host_config_t`'s `fifo_settings_custom` toward RX:
  **rx=512 / nptx=192 / ptx=0** lines (nptx 192 → OUT MPS limit ~768, ample for the
  512-byte bulk OUT; Tab5→phone is light). On device, advertise a modest CNXN
  maxdata (16 KB). **Second gotcha — the real one (cost a long mirror-stability
  hunt, then a wrong fix, then the right one):** at high bulk-IN throughput (the
  uncapped/GPU-offloaded mirror runs ~2-4 MB/s vs the old ~0.4) the P4 usb_host
  **intermittently corrupts a payload byte** → a malformed JPEG strip → a
  `jpeg.fullrange` HW decode error. The first hunt wrongly concluded "a DWC2
  *large-transfer* quirk" and mitigated it by reading the payload in ≤4 KB chunks —
  which only masked it at low rate. The **actual cause is RX-FIFO starvation**: the
  old rx=256-line (1 KiB = 2 packets) FIFO overflowed when the USB DMA drain
  stalled under sustained high-rate IN + concurrent PSRAM traffic (the MIPI-DSI
  panel refresh + JPEG decode). Proof it's not a transfer-size quirk: the
  `usb_host_uvc` reference streams clean **10 KiB** IN transfers on this same chip.
  **Fix (verified on a real Tab5 + an Android phone: 0 decode errors at ~40-49 fps):**
  the RX-biased FIFO above **plus** dropping the ≤4 KB chunking — `read_packet()`
  reads each A_WRTE payload in **one** transfer (adbd writes the payload with a
  single `usb_write`, so it arrives as one bulk-IN byte stream; the persistent
  `hdr_xfer_`/`data_xfer_` are reused + grown lazily, no per-packet alloc). A
  *UVC-style multi-in-flight transfer pool* was also tried (to keep the IN endpoint
  busy during JPEG decode) but it **hung the reader after ~2 s under load** —
  keeping N transfers in flight needs consumer-thread re-submission (cross-thread
  `usb_host_transfer_submit`) for ADB's lossless backpressure, which UVC sidesteps
  only because its in-callback re-submit can *drop* on overflow (fine for video,
  not for ADB). The single-transfer-per-payload reader is already clean at 40-49
  fps, so the pool's read-ahead wasn't the bottleneck; **not pursued.**
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
`write()` is non-blocking. **Listeners are split per TYPE/channel** (mirroring the
wire's TYPE multiplexing) so independent consumers attach to just their slice: the
link *owner* (`app::AgentClient`) passes a **`LinkLifecycleListener`** to `open()`
for `on_link_hello`/`on_link_close` (the connection state machine), and a *feature*
registers a **`VideoListener`** with `Link::set_video_listener(weak)` for
`on_mirror_started`/`on_video_strip`/**`on_orientation`** — held weakly, set/cleared
from any thread (guarded by `video_mtx_`), so the feature comes and goes while the
owner keeps the link alive. All fire on the **reader thread** (LVGL marshalling is
the app's job). The agent's **ORIENTATION `EVENT`** (TYPE=0x03, §4.4 — source
device logical rotation, sent at stream start + on each rotation change) parses to
`on_orientation(OrientationInfo{rotation, landscape})`; it rides the video channel
because the consumer (the mirror screen) uses it to lay its overlay out portrait vs
landscape — the video itself is unchanged (natural-orientation lock). Future AUDIO
channels add the same kind of `set_*` setter.
`Link::start_mirror(MirrorConfig)` sends the Tab5-initiated `MIRROR_START`
(non-blocking; call after `on_link_hello` once the video listener is registered);
`Link::stop_mirror()` sends **`MIRROR_STOP`** (§4.4) — the agent stops the JPEG
stream and returns to READY but the **link stays open**, so a later `start_mirror()`
resumes (this is what lets a feature stop without dropping the agent).
**Input injection (§4.7):** `Link::inject_key(keycode, action)` + the convenience
`Link::tap_key(keycode)` (down→up), and `Link::inject_touch(action, pointer_id, x, y)`,
send a **`TYPE=INPUT`** frame (Tab5→agent, **fire-and-forget — no req_id / no
response**, off the CONTROL_REQUEST handshake path so high-frequency touch never
waits). This is the shared input channel: `input_type` 0x00=KEY (the mirror
overlay's power / volume / nav buttons, via Android `KeyEvent.KEYCODE_*` constants
in `agent_link_protocol.hpp`) and 0x01=TOUCH (the mirror's touch passthrough —
per-pointer DOWN/MOVE/UP in **Tab5 panel coords**, `pointer_id` = the source's
touch track id; the agent inverts the mirror geometry to the source's logical
display coords and assembles the multi-pointer `MotionEvent` itself, scrcpy
`PointersState`-style); 0x02=TEXT/keyboard is reserved. The agent
injects via the hidden `InputManager.injectInputEvent` (scrcpy technique, shell
uid holds INJECT_EVENTS) in `android-agent/.../Input.java` (a minimal port of
scrcpy's `Workarounds.getSystemContext` → `getSystemService(INPUT_SERVICE)`). The same
parser carries the JPEG strip stream: each whole JPEG frame (the frame layer
reassembles A_WRTE splits) is handed to a **decode+framebuffer seam** =
`VideoListener::on_video_strip(VideoStrip)` (rect + JPEG bytes + frame_start/end),
so `Link` stays free of libjpeg / HW-JPEG (host libjpeg in the test, P4 HW JPEG +
bsp FB in the app). `tx_seq_` is atomic because `start_mirror`/`stop_mirror`
(app thread), `inject_key`/`inject_touch` (LVGL or touch-task thread), and the
HELLO response (reader thread) all write frames. **HELLO + Phase 2
mirror done & verified on a real Android device**:
`test/test_hello.cpp` covers the HELLO-only path (a `LinkLifecycleListener`);
`test/test_mirror.cpp` (a `LinkLifecycleListener` + `VideoListener`) drives HELLO →
`set_video_listener` → `start_mirror` → strips, asserts framing/16-alignment/tiling
via host libjpeg, then exercises **`stop_mirror` → `start_mirror` again on the same
link** (the resume cycle) (run with `nix develop -c sh -c 'TEST=test_mirror
components/agent_link/test/run.sh'`; default `TEST=test_hello`; the runner builds
the host stack incl. `libjpeg`, runs `adb kill-server`, launches the test;
artifacts in `test/build/`; approach in `android-agent/docs/testing.md`).
receive→decode→**render done**: the app drives the link via `app::AgentClient` +
`ADBMirroringScreen` (see the UI section) — `ensure_connected` → `set_video_listener`
→ `start_mirror` → `on_video_strip` decoded straight into the bsp framebuffer (no
LVGL), verified headless against a real Android device.

### The JPEG decode seam (for the mirror render)

`VideoListener::on_video_strip` hands the app a whole JPEG strip to decode into the
`bsp` framebuffer. The decode is **the same call on both targets** —
`jpeg_new_decoder_engine()` + `jpeg_decoder_process_full_range()` (+
`jpeg_alloc_decoder_mem()` / `jpeg_del_decoder_engine()`) — split below per the
two standard rules:

- The IDF JPEG **engine API** (`driver/jpeg_decode.h`) is Espressif's, so the
  host gets it in `simulator/idf_compat/` (libjpeg-backed). Device uses the real
  `esp_driver_jpeg`.
- `jpeg_decoder_process_full_range()` is **not** an Espressif API (it overrides
  the 2D-DMA CSC matrix registers for full-range BT.601 — MJPEG/JFIF content is
  full-range, but the IDF decoder bakes in the *limited-range* matrix and washes
  the image out). So it's the shared, `ESP_PLATFORM`-branched
  `components/jpeg_fullrange_decode/` component (device = the register-override
  driver; host = a passthrough to the libjpeg shim, which is already full-range).
  App code calls the one name and the simulator previews the fullrange-fixed
  device output. **Colour gotcha (cost several HW cycles):** for **RGB565 output**
  on the Tab5 panel use `rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR`, **not** `RGB`.
  The panel is driven R-in-high-bits RGB565, and on the P4 the *BGR* scramble is
  what produces that packing; the *RGB* enum's 565 scramble mis-packs the 16-bit
  pixel (the 6-bit green straddles the byte boundary) so the image renders as
  rainbow noise with the structure intact. (Matches the proven
  `Tab5-Screen-Streamer` decoder, same panel.) The simulator's libjpeg shim
  (`idf_compat/jpeg_decode.c`) mirrors this — its RGB565 BGR branch packs R-high —
  so the sim previews the device-correct colours with the same `dcfg`.

The API is **unchanged from the plain IDF `jpeg_decoder_process()` signature**
(tightly-packed output) — no stride/offset parameter. That is deliberate: the
**P4 JPEG 2D-DMA can only write a tightly-packed `process_h`×`process_v` picture**;
it cannot place a narrower picture into a wider buffer with a row stride. (Two HW
attempts to add that failed — HA-as-stride sheared the image with a black band per
strip, and the X/Y-offset descriptor form left only a thin band per strip. Both
crashed/misrendered on hardware.) So instead the **agent always streams
full-panel-width frames** (it bakes scale-fit + the black letterbox into a
720×1280 frame, see the agent section), each strip is the full panel width, and
the mirror decodes a strip straight into framebuffer row `strip.y` with
`decode_outbuf = fb + strip.y*pitch` — the strip width equals the framebuffer
pitch, so a tight decode *is* a placed decode, zero-copy, **and the framebuffer is
written exactly once** (no extra PSRAM-bandwidth blit). The stock per-call cache
sync (`decoded_buf`, `size = w*h*2`) is naturally aligned (`strip.y` and `h` are
16px multiples). Verified end-to-end in the simulator (and the headless
`test_mirror`) against a real Android device.

Two robustness measures live alongside the decode: `frame_ok_` drops any frame a
strip fails to decode (don't present a half-updated buffer), and
`jpeg_fullrange_decode_p4.c`'s error path soft-resets the JPEG FSM before
`dma2d_force_end()` so a decode error can't leave the DMA2D RX ISR to panic on a
non-idle FSM. (These mattered during a long mirror-stability hunt whose real cause
turned out to be the **usb_host transport corrupting large bulk-IN payloads** — see
the `embedded_adb` transport note; once that was fixed the decode errors vanished.)

### `app::AgentClient` (app-global tab5adb-agent manager)

`app/agent_client.{hpp,cpp}` — the app-global owner of the **tab5adb-agent
connection**, decoupled from any one screen so multiple features can use the agent.
Where `agent_link::Link` is the per-stream protocol engine, `AgentClient` owns the
**agent process lifecycle** + a lazy connection state machine — the same layering as
`adb::Client` (lifecycle) over `AdbConnection` (protocol). It deliberately does
**not** forward per-feature protocol; features drive the protocol on the `Link`
directly, so `AgentClient` never grows per-feature methods (the design choice that
keeps it from becoming a thin `Link` wrapper).

- **Lazy bring-up.** `ensure_connected(cb)` runs the §2.2 sequence on a private
  worker task — `exec` pkill stale agent → `Sync::push` the **embedded jar**
  (`app/agent/agent_jar.{h,c}`) → `open_shell` `app_process` → retry `Link::open`
  until the agent answers HELLO. It is the `SyncListener`/`ShellListener` for the
  push + agent stdout and the `LinkLifecycleListener` for the link. Nothing happens
  until the first `ensure_connected` (a feature's first use), not at adb connect.
  `cb` fires on the **LVGL thread** (like `adb_connect_async`): true once Ready,
  false on failure; already-Ready posts immediately; concurrent calls coalesce.
- **Persistence.** Holds the agent `Shell` (its stdout) + the `Link` alive across
  the transient screens. `state()`/`ready()`/`link()` are callable from any thread;
  `link()` returns a `shared_ptr<agent_link::Link>` (held across the call even if the
  link drops). Features get the video channel by `link()->set_video_listener()` and
  drive `start_mirror()`/`stop_mirror()` on it — `stop_mirror()` **keeps the link**,
  so a feature stops without dropping the agent (the whole point).
- **Teardown.** `on_link_close` (the agent died) and `on_adb_disconnected()` (the
  adb holder calls it on `Closed`) both go to Disconnected and drop the session
  objects — deferred via `lv_async_call` so the shared_ptr resets never run on the
  `Link`'s own callback stack — so a later `ensure_connected` re-launches.

The standard feature flow (see `ADBMirroringScreen`): `ready()` to decide whether to
show a waiting UI → `ensure_connected` → on the LVGL-thread callback
`set_video_listener(self)` + `start_mirror()`; on exit `stop_mirror()` +
`set_video_listener({})`.

The `app::agent_client()` singleton is a **deliberately-leaked** heap `shared_ptr`
(never destroyed): other statics call into it from their destructors at process exit
(e.g. `ScreenManager` tearing down a live `ADBMirroringScreen`, whose dtor calls
`agent_client().link()`), and cross-TU static destruction order is unspecified — a
normal Meyers static raced and `std::mutex::lock()` threw on the destroyed mutex.
The leak keeps the controlling ref (so `shared_from_this` works) for the whole
process; the device firmware never exits so nothing actually leaks.

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
protocol/transport details, only the `adb` component's typed surface. On `Closed`
the holder also calls `app::agent_client().on_adb_disconnected()` so the
tab5adb-agent connection is torn down with the adb link.

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

`ADBDeviceScreen`'s **Mirroring** button pushes **`ADBMirroringScreen`**
(`app/adb_mirroring_screen.*`) — the live screen-mirror viewer over `agent_link`.
The screen **is** the `agent_link::VideoListener`; the agent lifecycle (jar push +
`app_process` launch + HELLO) is **not** the screen's job — it belongs to the
app-global **`app::AgentClient`** (see the AgentClient section). `onEnter()` checks
`app::agent_client().ready()`: if the agent is already live it starts immediately
(no wait); otherwise it shows a centered **"Connecting…"** LVGL label and calls
`ensure_connected(cb)` (cb on the LVGL thread). On success `start_mirror_ui()` drops
the label, enters DM overlay mode, registers the screen as the link's video listener
(`link()->set_video_listener(self)`), and calls `link()->start_mirror()` (720×1280
fit, video). `onExit()` calls `link()->stop_mirror()` + `set_video_listener({})` —
**the agent link stays connected** so re-entering resumes instantly — then joins the
decode task and exits overlay. **Rendering bypasses LVGL
entirely** (the draw cost of compositing UI over a video stream is what we're
avoiding) and **receive and decode run on separate threads** so the blocking
HW-JPEG decode never stalls the adb reader thread — and thus the per-A_WRTE/A_OKAY
flow control that gates the next USB IN transfer (decode-on-the-reader-thread made
the device stall for every strip's decode latency, capping fps below the link's
real throughput; this is the `Tab5-Screen-Streamer` split, which kept its USB
receive free of decode). Two threads, handed slots through two FreeRTOS queues:
- **producer = the adb reader thread** (`on_video_strip`): copies each strip's
  JPEG bytes into the current **frame slot** (`FrameSlot.buf`, PSRAM, grown lazily;
  the HW-JPEG input DMA reads PSRAM directly and the fullrange decoder syncs the
  input cache `UNALIGNED`, so concatenated strips need no DMA bounce buffer and no
  per-strip alignment — **one fewer copy** than the old per-strip DMA input) plus a
  per-strip `{y,h,off,len}` descriptor; on `frame_end` it publishes the finished
  frame and returns, so the link acks immediately and the USB stream keeps flowing.
- **consumer = a private decode task** (`decode_loop`): drains finished frames,
  HW-JPEG-decodes each strip through the `jpeg_fullrange_decode` seam **straight
  into a bsp framebuffer**, presents it, recycles the slot. It owns `fb_`/`back_`
  exclusively (no lock).

Slot ownership moves `free_q_ → producer fills → ready_q_ → consumer decodes →
free_q_`, so exactly one thread touches a slot at a time; `ready_q_` is cap-1
(latest-frame-wins) and whoever pulls a slot out of it owns recycling it (the
producer reclaims a superseded frame's slot before publishing the next), so a slow
decoder **drops whole frames** instead of stalling the reader — and the producer
never blocks. The agent sends **full-panel-width** strips (scale-fit + letterbox
are baked in agent-side, see the agent section), so a strip's width equals the
framebuffer pitch and a tightly-packed decode into `fb + strip.y*pitch` lands in
place — zero-copy at decode, no scratch, no blit, the framebuffer written exactly
once (the P4 JPEG 2D-DMA can't stride a narrower picture into a wider buffer, so
full-width frames are what make decode-direct possible). The three bsp framebuffers
(`bsp_display_get_frame_buffer(0/1/2)`, `bsp_init` requests `fb_num=3`) are a
**triple buffer**: decode into the back one, `bsp_display_flush()` it (it becomes
the displayed/front buffer), advance `back_` to the next in the 0→1→2→0 rotation —
so the displayed buffer is never mid-write (tear-free) **and** the buffer drawn
into was last displayed two frames ago, so the decode task never blocks on the DPI
panel's scan-out/vsync sync before reusing one (normal full-screen LVGL still uses
just the first two as a double buffer; the simulator's SDL backend now allocates up
to 3 too). Nothing is marshalled to LVGL once streaming. Before the mirror starts
the LVGL root is **black** and hosts a transient **"Connecting…"** label (rendered
by the normal LVGL runtime while `AgentClient` brings the agent up); once
`start_mirror_ui()` enters DM overlay mode the label is gone and nothing invalidates
the root, so `lv_timer_handler` leaves the framebuffers to the mirror, while a small
LVGL overlay display renders the opaque control strip DM composites at flush time.
The **control overlay** is an icon-only strip anchored to the **viewer's bottom-left
corner**: a **vertical** strip in portrait that becomes a **horizontal** strip
(along the now 1280px bottom edge) when the source device turns landscape. It hugs
the panel corner (no margin), uses one button size for both orientations, and draws
thin separator lines between the button groups — driven by the agent's **ORIENTATION
event** (`VideoListener::on_orientation`),
which rebuilds the overlay in place by re-calling the **re-entrant** `enter_overlay()`
with the new footprint/rotation (a leaf FreeRTOS `overlay_mtx_` serializes the
buffer/geometry swap against the decode thread's compositor). The layout is keyed off
the device's **actual rotation** (`Surface.ROTATION_*` 0..3), not just portrait/
landscape: the overlay's PPA angle is `view_rot(rot)*90` and the footprint anchors to
the panel corner that becomes the viewer's bottom-left after the Tab5 is physically
turned (0→panel bottom-left, 90→top-left, 180→top-right, 270→bottom-right), so
ROTATION_90 vs _270 land on **opposite** corners. `in_corner` (the swipe hot zone)
tracks the same corner. `view_rot(rot) = (4-rot)&3` encodes the real-HW turn
handedness (verified reversed from the naive `rot` on a real device) and is applied to
**both** the angle and the corner so they stay in sync; flip it back to `rot` if a
later device shows the other handedness. Buttons
(placeholder lucide_40 icons) in the spec'd order — corner-outward: Hide,
Back/Home/Recents, Vol-/Vol+/Power, OpMode/DispMode/End. **Wired:** Hide (hides the
strip), End (pops to the device screen), and the six device buttons
**Back/Home/Recents/Vol-/Vol+/Power** — each injects a key tap on the source via
`agent_client().link()->tap_key(KEYCODE_*)` (§4.7 INPUT channel, fire-and-forget,
straight from the LVGL event since `tap_key` is non-blocking and the link is live
while streaming), plus **OpMode** = the touch-control toggle (**on by default**;
icon is the LVGL theme primary blue when on, white when off).
**DispMode stays a stub** pending its final UI. **Touch passthrough (§4.7):** when
OpMode is on, the screen's `on_touch` injects touches over the mirror to the source
as per-pointer MotionEvents via `agent_client().link()->inject_touch(action,
pointer_id, x, y)` (Tab5 **panel coords**; the agent inverts the mirror geometry).
`on_touch` keeps a small id-keyed table (`pass_[]`, guarded by `pass_mtx_`) and
diffs each touch-task snapshot into per-pointer DOWN/MOVE/UP, keying off the BSP
`bsp_touch_point_t.id` (the controller track id) so multi-touch needs no
id synthesis; each new pointer is classified once — **Pass** (injected), **Reveal**
(a corner-swipe candidate), or **Ignore** (over a visible strip / passthrough off) —
and keeps that role until it lifts. `onExit`/dtor + turning OpMode off call
`release_all_pointers()` to UP any still-down Pass pointer (no stuck finger on the
source). No timeout auto-hide: the in-strip **Hide** button hides it, and a **swipe
out of the anchor corner** (the **L-shaped** `in_corner` hot zone = two narrow
`kEdgeThick`-wide bands along the corner's two edges, kept narrow so it steals
little area from passthrough) reveals it again — detected in the same `on_touch`
(`DisplayManager::TouchListener`, registered via `set_touch_listener` in
`start_mirror_ui`, cleared on exit; fires on the touch task thread, see the
DisplayManager touch section), **not** a poll timer. The reveal calls
`consume_overlay_touch()` then `set_overlay_visible(true)` so the swipe gesture
itself isn't delivered as a tap to the just-revealed buttons. While OpMode is on
and the strip is visible, touches **outside** its footprint (`in_overlay_footprint`)
still pass through, so the user keeps operating the device without hiding the strip.
On pop the previous screen's full re-render reclaims the framebuffers.
`onExit()`/dtor stop the producer (`stop_mirror()` + `set_video_listener({})` — the
agent link is kept connected) **before** joining the decode task, so it can't flush
into a framebuffer the previous screen is reclaiming; the weak-listener `lock()`
keeps the screen alive across any in-flight strip so teardown frees the engine
race-free. The single-threaded decode path was **verified headless against
a real Android device** (`./run.sh simverify simulator/verify/mirror.txt`) and by the headless
`test_mirror`; the receive/decode split builds on both targets but its fps win is
the **remaining device flash-and-check** (the ~55fps RGB565/Q60 ceiling motivating
the split was the reader-thread serialization, not PSRAM bandwidth).

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

### Headless UI verification (the sim harness)

Automated, host-display-free UI checks instead of osascript clicking/screenshotting
the real SDL window (fragile, host-state-dependent, and hands the agent host-PC
control). The **same `simulator` binary** runs the **same app/BSP code**, just with
input/output redirected — two env vars, generic (not Tab5-specific) so other
boards reuse them:
- `SIMULATOR_HEADLESS=1` — `sdl_backend.c` skips the window/renderer/texture
  (SDL inits TIMER only, for LVGL's `SDL_GetTicks` tick) and keeps only the
  in-memory framebuffers. LVGL still renders into `s_fb` in DIRECT mode, so a
  finished frame is fully present with no window; runs in non-interactive
  shells / CI.
- `SIMULATOR_SCRIPT=<path>` (`-` = stdin) — `main.cpp` runs
  `sim_harness_run()` (in `simulator/platform/sim_harness.cpp`) instead of the
  interactive loop. A tiny line-oriented interpreter that runs **on the main/LVGL
  thread** (so capture/input never race rendering): `wait <ms>`,
  `settle [<max_ms>]`, `capture <path.jpg>`, `tap <x> <y>`, `down <x> <y>`,
  `move <x> <y>`, `up`, `quit`. `settle` pumps `lv_timer_handler` until no
  animation runs for a few frames (drains `lv_async_call` work), so capturing
  before the frame settles is the caller's mistake. `tap` = inject press →
  hold a few frames → release → hold a few frames, so each edge spans both a
  background touch-task sample (which feeds the indev) and an LVGL indev read, and
  the click registers; `down`/`move`/`up` build drags/swipes.

Layering split: the **primitives** live in the SDL backend (the hardware seam) —
`sdl_backend.c` honours `SIMULATOR_HEADLESS`, reports the harness's injected
pointer from `touch_read` (`sdl_backend_inject_down`/`_up` write the touch
snapshot; headless has no mouse), and exposes `sdl_backend_snapshot()` (the
most-recently-flushed framebuffer + geometry/format, RGB565). `touch_read` now
runs on the DisplayManager touch task (a background thread), so the touch snapshot
is **mutex-guarded** and the main thread maintains it (`sdl_backend_pump_input()`
samples the mouse; the harness injects directly) — `touch_read` never calls SDL. The
**orchestration** (script loop, `lv_timer_handler` pumping, JPEG encode via the
already-linked libjpeg) lives in the platform-layer harness — so the BSP stays
image-format-agnostic. The agent's loop becomes: write a script → run headless →
`Read` the captured JPEG. Examples: `simulator/verify/home.txt` (capture),
`simulator/verify/touch.txt` (tap → screen transition); `capture` auto-creates
its output's parent dir, and the examples write to the gitignored
`simulator/verify/out/`. Run via `./run.sh simverify <script>` (builds, then runs
headless — sets both env vars), e.g. `./run.sh simverify simulator/verify/home.txt`.
**Done & verified: headless rendering, framebuffer capture, and synthetic
touch** (tap registers a click → real screen navigation). Next: a `run.sh`
mode / per-screen verify scripts as the UI grows.

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
  - `driver/jpeg_decode` — the IDF JPEG decode engine API
    (`jpeg_new_decoder_engine`/`jpeg_decoder_process`/`jpeg_alloc_decoder_mem`/
    `jpeg_del_decoder_engine` + cfg/enum types), backed by **libjpeg**
    (`src/jpeg_decode.c`). RGB565/RGB888 out only; libjpeg decodes JFIF
    **full-range**, which is what the device's `jpeg_fullrange_decode` reproduces
    (see that component). pkg `libjpeg` links to the `simulator` exe in
    `simulator/CMakeLists.txt`.
  - `driver/ppa` — the IDF PPA (Pixel-Processing Accelerator) API
    (`ppa_register_client`/`ppa_do_scale_rotate_mirror`/`ppa_do_blend`/
    `ppa_do_fill` + `hal/ppa_types.h`, `hal/color_types.h`), a **CPU software
    impl** (`src/ppa.c`) so app code that offloads scale/rotate/mirror/blend/fill
    to the P4 PPA HW previews in the sim. Supports the RGB display modes
    (ARGB8888/RGB888/RGB565, + A8/A4 blend fg); YUV420/444 and blend color-keying
    return `ESP_ERR_NOT_SUPPORTED`. Bilinear (anti-aliased) scale, CCW rotation,
    ops run synchronously (any `PPA_TRANS_MODE_*`). Header surface mirrors IDF verbatim.
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
