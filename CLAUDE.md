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
back to waiting for the next MIRROR_START (READY) on the same socket. A
**MIRROR_START arriving *mid-stream* reconfigures in place**: `streamVideo` also
breaks when `conn.pendingStart != null` (set by `handleMirrorStart`), and the
session loop restarts it with the new params — so the Tab5 switches scale/display
mode by sending one fresh MIRROR_START, no stop required (the basis for the
mirror's DispMode toggle; a stop+start race could hang). Frame writes
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
letterbox). **`scaleMode` fit vs fill (§5.3) is honoured on both paths.** The
fallback drives it directly in `Projection.compute` (a negative dest offset =
center-crop). The primary mirror only ever aspect-*fits* into its reader surface,
so **fill** is done by *oversizing the reader*: `Projection.fillCover` sizes it to
the natural-orientation cover rectangle the source fills exactly (no letterbox),
then the centered `targetW×targetH` panel crop is taken as the strip read-origin
(`ScreenCapture.cropX/cropY` → `FramePipeline.stripsOf(frame, cropX, cropY)`) — **no
extra per-frame copy**, since stripping already sub-bitmaps each band. Fit keeps a
panel-sized reader + a `(0,0)` crop (the zero-copy fast path). Verified on a real Android device
(Android 14/15 primary path): fit letterboxes, fill full-bleeds + crops.
**Physical-orientation lock
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
a device. The agent paces at `Server.TARGET_FPS = 60` (panel rate), further
bounded by the Tab5-requested **`max_fps`** (`MIRROR_START` args +8, append-only;
0 = uncapped — the DeviceScreen preview asks for 10); a static screen yields no
new `ImageReader` frame, so nothing is sent and the Tab5 keeps the last frame.
**`MIRROR_START`'s `target_w/h` is the viewer surface** (not necessarily the
panel; 16-aligned only when split), and the appended args also carry
**`jpeg_quality`/`split_count`** (0 = agent defaults 80/4; the preview asks for
60/1 — `split_count=1` streams each frame as ONE whole JPEG, no strip banding,
which drops the 16px alignment requirement). **`scale_mode=2 aspect`** makes the
agent size the output itself: the source's natural aspect fitted into that box
(`Projection.aspectOutput`, even-rounded — the bound dimension lands on the box
edge exactly, so a portrait phone streams at the full 360 preview width), then
streamed as a plain fit into the resized box — full-bleed, with the chosen size
returned in the response's
appended `out_width`/`out_height` (the wire basis of the agent-based device
preview, see `AgentPreview`). The agent also serves **GET_APP_LIST /
GET_APP_ICON** (cmd 0x20/0x21, HELLO caps bit 2 = `APPINFO`): `AppInfo.java` over
the `PackageManager` reached via `SystemContext.java` (the scrcpy system-context
workaround, extracted out of `Input`; both services are built once on the **main**
thread at startup — `new ActivityThread()` needs its Looper — and HELLO drops
APPINFO if the PackageManager is unreachable). Labels come back agent-sorted
(case-insensitive, NBSP→space — Tab5 fonts lack the glyph); icons are drawn onto
a Canvas at size×size (no text → no
Typeface pitfall) and returned **raw ARGB8888** — Android Color ints written LE
are exactly LVGL's native ARGB8888 byte order (B,G,R,A), so neither side
converts. **Two icon gotchas (cost a debug session, verified on the real Android device):**
(1) for **split-APK installs** the launcher icon bitmaps live in
`split_config.<dpi>.apk` and `getApplicationIcon` under the synthetic context
falls back to the framework default icon, which *also* fails to load — `AppInfo`
builds its own `Resources` over base + all splits via the hidden
**`ApkAssets.loadFromPath` + `AssetManager.setApkAssets`** (which merges
same-package-id tables across splits; the legacy `addAssetPath` does NOT — each
split stays a separate package and density values never resolve) and reads
`getDrawableForDensity(icon, DENSITY_XXHIGH)`; (2) inflating those icon XMLs hits
framework code calling `ActivityThread.currentApplication().getResources()`,
which NPEs while `mInitialApplication` is null — `SystemContext` installs
scrcpy's **fillAppContext** (a bare `Application` wrapping the system context).
The Tab5-side
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
  terminal/                  #   ADBShellScreen's terminal widgets: term_view (cell-grid renderer) + term_keyboard
  test/                      #   host unit tests (device_info / apk_info parsers) + run.sh — no phone, no LVGL
components/                  # SHARED  (both targets)
  m5stack-bsp/               #   board support (bsp_*) — device drivers + SDL sim backend
    inc/                     #     model-agnostic public API (bsp.h incl. bsp_audio_*) + audio_dsp.h, bsp_sd.h, bsp_types.h
    inc_private/             #     internal driver interfaces (bsp_display.h / bsp_touch.h / bsp_audio.h vtables)
    src/                     #     shared dispatch: bsp_display.c/bsp_touch.c/bsp_audio.c + audio_dsp.c
    devices/                 #     DEVICE reusable chip drivers (ili9881c/st7123/gt911/es8388/pi4io)
    simulator/               #     SIM reusable SDL backends (sdl_backend.c display/touch, sdl_audio.c) + sd_redirect.c
    boards/<model>/          #     per-model bring-up: <model>.c (device, + tab5_audio.c, tab5_sd.c) + <model>_sim.cpp (sim)
    test/                    #     host unit tests (audio_dsp math, sdl_audio pacing) + run.sh — no device
  lvgl++/                    #   C++ helpers (lv_async_call, lv_obj_add_event_fn — DELETE-filtered handlers run before cleanup)
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
  wifi/                      #   Wi-Fi STA connection manager (scan/connect/state) — prerequisite for ADB-over-TCP
    inc/                     #     wifi_manager.hpp (Manager/Listener/Status/Result) + wifi_sim.hpp (sim fake control)
    src/                     #     wifi_manager.cpp (portable) + backend_espwifi.cpp (device) / backend_sim.cpp (sim); wifi_backend.hpp seam
    README.md  docs/         #     README = front door; docs/wifi.md = surface detail
  jpeg_decode_enhanced/      #   enhanced P4 HW JPEG decode (full-range CSC, strip pipeline, PPA); ESP_PLATFORM-branched
    include/  src/           #     Layer 1 (jpeg_decode_enhanced.h) + Layer 2 (jpeg_ppa_pipeline.h); _sim.c = libjpeg/SW-PPA-backed (see its README)
  term_emu/                  #   VT100/xterm-subset terminal emulator (bytes in -> Cell grid out); no LVGL/adb deps
    inc/  src/  test/        #     term_emu.hpp; vt_parser.cpp (FSM) + term_emu.cpp (grid); host test (run.sh, no phone)
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

**Audio (`bsp_audio_*` in `bsp.h`)** follows the same three layers and is
**capability-based** so every M5Stack audio variant (no audio / buzzer-only /
speaker-only / speaker+HP) fits one API: `bsp_audio_get_caps()` returns
`BSP_AUDIO_CAP_{PCM,TONE,SPEAKER,HEADPHONE}` bits and unsupported calls return
`ESP_ERR_NOT_SUPPORTED` (no provider = caps 0). **`bsp_init` outputs no
signal**: providers register *closed* (DAC down, amp gate off; `bsp_config_t`
carries only `audio.dsp_mode` + `audio.speaker_mode` — the stream format is
`bsp_audio_open()`'s argument, and write/reconfig/close before open are
`ESP_ERR_INVALID_STATE`). The shared dispatch (`src/bsp_audio.c`) owns all
**policy**: the user volume curve (linear-in-dB, -40 dB span, delivered as a
fading software gain; settable before open, applied by the open fade-in), mute
(a SW fade, never a HW step), the speaker route ON/AUTO/OFF policy + the
generic headphone poll task (200 ms, notify-woken; AUTO needs CAP_HEADPHONE) +
insert callback, the **DSP voicing mode**, and the **click-free sequencing
contract** (stated in `inc_private/bsp_audio.h`): amp state changes only while
the DAC is settled silence, audible amplitude changes only via the SW fade —
the **first open arms the amp** (silent gain → codec unmute @ max HW volume →
~50 ms analog settle → amp on per mode); open/reconfig fade the stream in from
0; close HW-mutes before the clocks stop but **keeps the amp** (so the amp
transient isn't re-paid per open); `bsp_audio_quiesce()` (mute + amp off) is
the `bsp_restart()` path. **DSP modes** (`bsp_audio_dsp_mode_t`, zero-init =
**Auto**): *Auto* = the board's voicing, pulled from the provider's
`get_dsp_profile(headphone, sample_rate)` vtable hook (coefficients designed
at the live rate) and **re-applied on HP insert/remove** by the route task —
app edits get overwritten; *Manual* = DSP initialised flat, app drives it via
`bsp_audio_dsp()`; *Disable* = no DSP (`bsp_audio_dsp()` NULL, volume falls
back to the HW codec, clicky). Tab5's voicing (`tab5_audio.c`): speaker = HP80
+ low-shelf 300/+7 dB + peak 150/+3 dB + mono mix (only L is wired); headphone
= HP50 + low-shelf 150/+10 dB + peaks 1 k/-4 dB, 2.5 k/-3 dB, stereo.
Providers implement only low-level vtable ops (open/close/write/set_hw_volume/
set_hw_mute/set_speaker_enabled/headphone_inserted/get_dsp_profile/tone,
optional = NULL): Tab5 = `boards/tab5/tab5_audio.c` (ES8388 + PI4IOE1 SPK_EN
pin 1 / HP_DET pin 7, caps PCM|SPEAKER|HEADPHONE; `es8388_init` no longer
opens the codec dev — `es8388_open` does); simulator = `simulator/sdl_audio.c`
(SDL queue-audio, caps PCM|SPEAKER) whose `write()` **backpressures at ~100 ms
queued** to mimic the blocking I2S DMA write — under `SIMULATOR_HEADLESS` (or
no host audio device) it degrades to a silent **null sink with the same
real-time pacing**, so verify runs exercise producer timing.
EQ/post-processing is **`audio_dsp`** (`inc/audio_dsp.h` + `src/audio_dsp.c`,
both targets, board-independent): a fixed chain EQ (cascaded RBJ biquads) →
gain (per-frame fade — the click-free volume primitive; fades continue
seamlessly across `process()` chunk boundaries) → stereo→mono mix, each stage
independently toggleable, whole-chain bypass fast path; stage capacity
defaults to 5 and **`set_biquads` auto-grows it** (beyond 8 stages `process()`
holds the lock instead of stack-snapshotting — the grow realloc would dangle
the direct pointer). The dispatch owns one instance per PCM provider (created
at boot at a nominal 48 k so the handle is valid pre-open, reconfigured per
open; 16-bit streams only — other widths bypass to HW volume); `set_gain`
belongs to the volume/mute plumbing. Host tests: `nix develop -c
components/m5stack-bsp/test/run.sh` (`TEST=test_audio_dsp` default — pure DSP
math; `TEST=test_bsp_audio` — dispatch policy vs a stub provider: voicing
modes, amp arming, HP re-voicing; `TEST=test_sdl_audio` plays an audible sine
through dispatch+provider and asserts the pacing, null-sink clean). Device
pop-noise behavior (boot/open/close/volume/HP-plug), the close-path settle
delays, and esp_codec_dev's deferred-open path still need a **real-HW flash
check**.

**SD card (`inc/bsp_sd.h`)** — mount/unmount only; once mounted, app code uses
plain POSIX file I/O under the mount point on both targets (no further BSP
seam). `bsp_sd_mount(mount_point, cfg)` / `bsp_sd_unmount()` /
`bsp_sd_is_mounted()`; mount on demand, treat `ESP_ERR_INVALID_STATE` as
"already mounted", no hot-plug detection (a failed scan unmounts so the next
Refresh re-mounts). Device (`boards/tab5/tab5_sd.c`): the TF slot is on the
P4's SDMMC, routed like the P4 EV board — CLK=G43, CMD=G44, D0..D3=G39..G42,
4-bit, card power from the **on-chip LDO channel 4** (`sd_pwr_ctrl_new_on_chip_ldo`,
kept acquired across remounts), default 40 MHz high speed;
`esp_vfs_fat_sdmmc_mount` (PRIV_REQUIRES `fatfs sdmmc esp_driver_sdmmc`).
sdkconfig: `CONFIG_FATFS_LFN_HEAP` (long file names — the default LFN_NONE
truncates to 8.3), `CONFIG_FATFS_API_ENCODING_UTF_8`, and
`CONFIG_FATFS_VFS_FSTAT_BLKSIZE=4096` (stdio buffering). **Read-performance
rule:** plain `fread` is slow on this path — read with unbuffered `read()` in
16 KB chunks into a `heap_caps_malloc(..., MALLOC_CAP_CACHE_ALIGNED)` buffer
(what the APK install push source does). Simulator
(`simulator/sd_redirect.c`): "mount" maps the mount point onto a host
directory (`SIMULATOR_SDCARD_PATH` env var, default `simulator/sdcard/` from
the repo root) by defining `open`/`fopen`/`opendir`/`stat` in the executable —
statically-linked calls resolve there, the path is translated, and the real
libc is reached via `dlsym(RTLD_NEXT, ...)` (NameCardKnot's approach). Device
SDMMC bring-up is **not yet verified on real HW** (sim path verified headless);
first flash should check the `BSP_SD` mount log.

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
- `transport` — the byte pipe under the protocol engine. Two kinds: **USB** (the
  device/simulator split) and **TCP** (one impl, both targets). USB bulk transfer
  to the ADB interface (USB class `0xFF` /
  subclass `0x42` / protocol `0x01`) is the **only device/simulator split**:
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
  HW JPEG decode error. The first hunt wrongly concluded "a DWC2
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
  **Third gotcha — already-plugged enumeration (verified on a real Tab5 + an Android phone):**
  the host stack is installed **lazily** (only when the user taps Connect, inside
  `open_usb_transport`), but the Tab5's host-port VBUS is the PI4IO **USB5V_EN**
  load switch (`pi4ioe2` pin 3), not the DWC2's internal DRVVBUS. If VBUS is on
  before `usb_host_install`, a device plugged in **before Connect** is already
  attached (D+ pulled up) when the root port powers, the DWC2 sees no
  idle→connected edge, and enumeration never starts (`enumerated devices: 0`
  forever — the 20 s poll can't help). Toggling the controller's internal
  root-port-power (`usb_host_lib_set_root_port_power`) does **NOT** fix it: the
  phone's physical VBUS doesn't cycle, so its line state never changes (tried,
  failed). **Fix:** *after* `usb_host_install` the transport **resets the host
  port** via an app-supplied hook (a real **USB5V_EN off→200 ms→on** power cycle) —
  the phone sees VBUS drop and rise = a genuine re-attach into an already-ready
  host = the connect edge the controller needs, so "already plugged" becomes
  identical to "plug after Connect". **API split (board-agnostic):** embedded_adb
  knows only **"reset the port"** — `adb::set_usb_host_reset_hook(void(*)())`, the
  transport calls it once after install (the libusb/sim transport never does).
  *What* a reset is lives in `adb_app` (the integrator): the registered hook
  composes the VBUS power-cycle out of the BSP primitive
  `bsp_usb_host_set_power(bool)` (tab5 = PI4IO USB5V_EN, sim = no-op). The boot VBUS
  state follows the persisted **USB Power** setting (`app::usb_host_power()`,
  Settings → Android Device → USB Power): **Always** (default) = `config.usb.usb5v_en = true`
  (VBUS on at boot/idle, a plugged phone charges); **Connected** = VBUS off while ADB
  is disconnected, powered only for the live link. `adb_app::apply_usb_host_power()`
  re-applies the policy for the current connection state (on at `on_state(Online)`,
  off at `on_state(Closed)` when the policy is Connected, and on the Settings toggle);
  connecting still re-powers the port via the reset hook regardless, so a device
  plugged before Connect enumerates on the rising VBUS edge either way. embedded_adb
  has no VBUS/on-off concept and no embedded_adb → bsp dependency.
  **Fourth gotcha — teardown must fully uninstall or a reconnect fails
  `usb_host_install: ESP_ERR_INVALID_STATE`:** the transport runs two background
  event tasks (`lib_task` = `usb_host_lib_handle_events`, `client_task` =
  `usb_host_client_handle_events`). `close()` (Disconnect button → `Client::close`
  → transport dtor) must **deterministically join both before uninstalling** — they
  give a per-task done semaphore (`lib_done_`/`cli_done_`) as they exit, and
  `close()` does: unblock + join `client_task`, `usb_host_client_deregister`, unblock
  + join `lib_task`, `usb_host_device_free_all`, then `usb_host_uninstall` in a retry
  loop that **pumps `usb_host_lib_handle_events` itself** (the lib task is gone, and
  uninstall only succeeds once the NO_CLIENTS / ALL_FREE events are processed). The
  old code only `vTaskDelay(50ms)`'d and ignored the uninstall return → it failed
  silently, the stack stayed installed, and the next Connect's `usb_host_install`
  returned `ESP_ERR_INVALID_STATE`. (Device-only path — the sim's libusb transport
  can't reproduce it; needs a real-HW flash check.)
- `transport_tcp.cpp` — **ADB-over-TCP** (the "Wireless (TCP/IP)" path). ADB's wire
  protocol is identical over TCP — the same 24-byte header + payload, streamed on a
  socket — so this is the **one transport that is the SAME on both targets** (lwip on
  device, the host's BSD sockets on the simulator): it is in the component's common
  sources, **not** the `ESP_PLATFORM` split, and needs no `idf_compat` shim (sockets
  are a standard contract, not an Espressif API). `open_tcp_transport(host, port)`
  does a non-blocking connect with a 5 s timeout (a wrong target fails fast), sets
  `TCP_NODELAY` (ADB is request/response-heavy) and `SO_RCVTIMEO` 1 s (so the read
  loop wakes to check `stop()`); `read_packet` loops `recv` until a whole packet is
  read (a stream, unlike USB bulk), reporting `Timeout` only at a packet boundary.
  **Two flavours, both verified on a real Tab5-vs-Android-phone path (in the simulator over
  the host network):** (1) **classic `adb tcpip 5555`** — same RSA AUTH as USB, works
  directly; (2) **Android 11+ wireless debugging** — the device replies **`A_STLS`**
  (STARTTLS) to our CNXN, and the connection upgrades to **TLS 1.3** before the
  banner exchange. `Transport::start_tls(const RsaKey&)` (a base no-op; only the TCP
  transport implements it) presents a **self-signed X.509 cert built from the same
  adb RSA key** (`RsaKey::self_signed_cert_der`, mbedTLS, fixed wide validity since
  the device has no RTC) and runs a mutual-TLS handshake (authmode NONE — we don't
  verify the device's ephemeral cert; adbd authenticates *us* by the client cert).
  After the handshake `write_packet`/`read_packet` route through `mbedtls_ssl_*` with
  the same boundary/Timeout semantics. `AdbConnection::on_stls` sends the `A_STLS`
  reply on the plaintext socket, then calls `start_tls`; the device then sends CNXN
  over TLS → Online, with **no RSA AUTH challenge** (auth is the cert). **A
  USB-authorized key is accepted for wireless-debugging TLS with no separate
  pairing** (verified). The TCP transport advertises the full 256 KB CNXN maxdata on
  both targets (no per-payload DMA alloc like usb_host). **Both builds compile.**
  ESP-IDF's mbedTLS has x509 cert *writing* on by default but **TLS 1.3 off**, so
  `esp32p4/sdkconfig.defaults` sets `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y` (the transport
  requests min=max=TLS1.3; +~64 KB binary). One device-mbedTLS gotcha: the deprecated
  mpi `mbedtls_x509write_crt_set_serial` is removed there — use
  `mbedtls_x509write_crt_set_serial_raw` (works on both). The on-device TCP/TLS path
  still needs a real-HW flash-and-check (sim-verified against a real Android device over the host
  network). Host test: `TAB5ADB_TCP_TARGET=host:port TEST=test_connect_tcp nix
  develop -c components/embedded_adb/test/run.sh` (env-supplied target → no IP in git;
  classic and wireless ports both pass).
  **Throughput tuning (mirror over WiFi):** on a high-RTT WiFi link TCP throughput is
  `window/RTT`, and the IDF default lwip window (5760 B) caps the mirror hard (the bulk
  direction is receive). `esp32p4/sdkconfig.defaults` widens it to Espressif's
  esp-hosted P4 reference values: `LWIP_TCP_WND_DEFAULT=65534` /
  `LWIP_TCP_SND_BUF_DEFAULT=65534` (the max non-scaled 16-bit window — no
  `LWIP_WND_SCALE` needed), `LWIP_TCP_RECVMBOX_SIZE=32`, `LWIP_TCPIP_RECVMBOX_SIZE=64`.
  **`LWIP_IRAM_OPTIMIZATION` is deliberately OFF** (the reference sets it, but it moves
  ~15 KB of LWIP code from flash into internal SRAM, and esp-hosted 2.x already eats
  most internal RAM at init — enabling it makes the boot-time FreeRTOS timer-task stack
  alloc fail with `vApplicationGetTimerTaskMemory ... pxStackBufferTemp != NULL`.
  Internal RAM is the binding constraint, not LWIP code locality).
  `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`
  stays (lwip/WiFi buffers in PSRAM, fine on the 32MB part; it also drives the
  `WIFI_RMT` static-TX buffer design). **Gotcha:** a `>65535` `WND_DEFAULT` would
  require `LWIP_WND_SCALE` (which `depends on SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, else it's
  silently absent and the window is rejected back to 5760) — the earlier 128000+scaling
  config did that, but the 65534 reference window sidesteps scaling entirely.
  `sdkconfig` is gitignored, so after editing `sdkconfig.defaults`
  you must `rm esp32p4/sdkconfig && idf.py -C esp32p4 reconfigure` (defaults are applied
  only when sdkconfig is absent) — `grep LWIP_TCP_WND_DEFAULT esp32p4/sdkconfig` to
  confirm it took. The remaining ADB-layer ceiling is the **classic per-OKAY
  stop-and-wait** (one A_WRTE per RTT); removing it needs the `delayed_ack` feature
  (banner feature + A_OPEN.arg1=window + 4-byte acked-bytes in A_OKAY) — not yet done.
  TCP tuning is a config-only change, **still pending a real-HW flash-and-check.**
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
plus `stat` (STAT) as its verifier, then `list` (LIST) for the browse UI, then
**Android→Tab5 `pull` (RECV)** for the copy-to-SD flow — a push-style `SyncSink`
consumes the DATA stream on the worker thread; a sink abort **closes the whole
session** (`Error::Cancelled` — RECV has no wire-level cancel), which is why
transfer jobs open their own `Sync` instead of borrowing a browser's. The
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
phone: push a buffer → stat it back → pull it back byte-identical, plus the
FAIL (missing path → `Rejected`) and abort (→ `Cancelled` + session close)
paths). Later slices add `screencap` and pull-into-memory preview.

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
`MirrorConfig` carries the viewer-surface size (16-aligned, not necessarily the
panel), `scale_mode` (incl. `kScaleAspect` — the agent picks the output size to
the source aspect) and `max_fps`; the response's `MirrorInfo` carries the
agent-chosen `out_width/out_height` (size receive buffers from these).
`Link::stop_mirror()` sends **`MIRROR_STOP`** (§4.4) — the agent stops the JPEG
stream and returns to READY but the **link stays open**, so a later `start_mirror()`
resumes (this is what lets a feature stop without dropping the agent).
**Generic one-shot control**: `Link::request(cmd, args, args_len, cb, timeout_ms)`
sends any registry CONTROL_REQUEST (GET_APP_LIST / GET_APP_ICON / future cmds)
with req_id correlation handled inside (ids 0x10.. cycle, clear of the fixed
mirror ids); the completion fires exactly once on the reader thread — on the
response (err=Ok + wire status), on link close (StreamClosed, all pending fail),
or on a lazily-swept timeout (sweeps run on link traffic / the next request, so
a fully idle link defers expiry until the close).
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
`PointersState`-style). **Batched touch (`kInputTouchBatch`, input_type 0x03) for
slow links:** touch is small-but-high-frequency, the pathological case for ADB's
per-A_WRTE/A_OKAY stop-and-wait — every event is a full transaction (header + RTT
round-trip + the single serialized `write_mtx_` send) that contends with the
inbound video's flow control, so processing touch drags the whole link down.
`Link::inject_touch_batch(samples, n)` packs N per-pointer transitions into ONE
INPUT frame (count + n×{action,ptr,x,y}); the agent replays them in order through
the same per-pointer state machine as `inject_touch` (semantically identical, one
A_WRTE). The mirror batches the MOVEs that pile up while the link is mid-round-trip
and flushes when it goes idle (see `ADBMirroringScreen`), cutting a fast drag from
one frame per touch sample to ~one per RTT — no points dropped, no added latency.
The idle signal is **`Link::tx_pending_bytes()`** → **`adb::Stream::pending_bytes()`**,
which now counts the **in-flight (dequeued-but-unacked) frame** too — so `== 0`
means the link is genuinely idle (the last A_WRTE's A_OKAY arrived), the precise
"channel free" gate batching needs. `inject_touch` (single) stays for non-batched
callers; the agent parses both input_types. 0x02=TEXT/keyboard is reserved. The agent
injects via the hidden `InputManager.injectInputEvent` (scrcpy technique, shell
uid holds INJECT_EVENTS) in `android-agent/.../Input.java` (a minimal port of
scrcpy's `Workarounds.getSystemContext` → `getSystemService(INPUT_SERVICE)`).
`Server.handleInput` dispatches on input_type: KEY (0x00), TOUCH (0x01), and
TOUCH_BATCH (0x03) — the batch handler just loops the records through the same
`handleTouch` path as a single TOUCH, so `Input.java` is unchanged. The same
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

### The `wifi` component (Wi-Fi STA connection manager)

Gets the Tab5 onto a LAN — the prerequisite for the future **ADB-over-TCP**
("Wireless (TCP/IP)") path. ADB-over-TCP itself is **not** this component: once an
IP is up it is a plain socket and belongs in `embedded_adb` (a future
`transport_tcp.cpp` + `adb::Client::connect_tcp()`), independent of `wifi`.

**Tab5 has no radio**: Wi-Fi is an **ESP32-C6 co-processor** over SDIO via
`esp-hosted` + `esp_wifi_remote`. `esp_wifi` calls are API-compatible and routed
to the C6, and `esp_wifi_init()` brings the SDIO transport up itself — driven
**entirely by sdkconfig** (SDIO pins / reset GPIO / slave target), NOT by C code.
The Tab5 wiring lives in `esp32p4/sdkconfig.defaults` (reset GPIO 15 active-low,
CMD=13 / CLK=12 / D0..D3 = 11/10/9/8, 4-bit @ 40 MHz, `esp32c6`), mirrored from the
`tab5remote` reference project. **Host esp_hosted is pinned to 2.12.6 +
esp_wifi_remote 1.6.1** in `components/m5stack-bsp/idf_component.yml` to match the C6
slave firmware (the slave and host esp_hosted versions must match — a 1.x-host/2.x-slave
mismatch ran very slowly). esp-hosted 2.x reworked the SDIO Kconfig: the old
`CONFIG_ESP_HOSTED_SDIO_PIN_*` keys became non-settable *derived* values, so pins are
set via the per-slot PRIV symbols (`CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_*_SLOT_1`) plus
`CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6` / `..._P4_DEV_BOARD_NONE` / `..._SDIO_SLOT_1`.
The receive-heavy mirror also bumps the `esp_wifi_remote` RX path
(`CONFIG_WIFI_RMT_RX_BA_WIN=32`, `..._STATIC_RX_BUFFER_NUM=32`,
`..._DYNAMIC_RX_BUFFER_NUM=64`; TX stays static — `WIFI_RMT_DYNAMIC_TX_BUFFER` is
Kconfig-disabled when `SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`). So **the BSP owns no Wi-Fi code** — the old
`bsp_config.wifi` / `esp_netif`/`esp_wifi_init` block was removed; standard ESP-IDF
network lifecycle is this component's job (the same way `adb` owns the USB/esp
lifecycle, not the BSP).

**Layering = the project's ONE-split pattern.** Portable C++
(`src/wifi_manager.cpp`: state machine, NVS persistence of the last network,
connect timeout via a FreeRTOS one-shot timer, listener dispatch) over a single
device/simulator backend split through the private `Backend`/`BackendHost` seam
(`src/wifi_backend.hpp`) — the SAME reasoning as `embedded_adb`'s usb_host
transport: `esp_wifi` is too large to reimplement on the host, so it is an
`ESP_PLATFORM`-branched backend (`backend_espwifi.cpp` = esp_wifi/esp_netif/
esp_event; `backend_sim.cpp` = a deterministic scriptable fake), **not** an
`idf_compat` esp_wifi shim. simverify must stay deterministic (no real
host-network access), which the fake provides. **C6-firmware crash gotcha:**
updated ESP32-C6 esp-hosted firmware fires the STA `WIFI_EVENT_STA_START`/`STOP`
events **redundantly (or while the netif is already up)**, and IDF's default
`wifi_default_action_sta_start` (= `wifi_start`, registered by
`esp_netif_create_default_wifi_sta()`) crashes on the second invocation (double
`esp_netif_action_start` / rxcb re-register). So `backend_espwifi.cpp` does **not**
call `esp_netif_create_default_wifi_sta()`: it creates the STA netif manually
(`esp_netif_new(ESP_NETIF_DEFAULT_WIFI_STA())` + `esp_netif_attach_wifi_station`)
and registers its own **idempotent** start/stop handlers (a `wifi_start` copy
guarded by `s_sta_netif_started || esp_netif_is_netif_up`), a connected handler
(rxcb register when `!esp_wifi_is_if_ready_when_started`), and IDF's public
`esp_netif_action_disconnected`/`esp_netif_action_got_ip` for the rest. This is the
STA analogue of M5Tab5-UserDemo's `fix wifi init crash with updated c6 firmware`
(which patched the AP path only — that demo is SoftAP-only). The fix is C-side only;
it is version-independent and kept as a defensive guard even now that the host
esp_hosted matches the 2.x slave. **Device-only — not reproducible in the sim fake;
needs a real Tab5 + C6 flash check.**

**API (`inc/wifi_manager.hpp`, see README + docs/wifi.md).** `wifi::manager()` is a
leaked singleton (like `app::agent_client()`). The blocking radio ops (bring-up,
on/off) run on **one Manager-owned worker task + command queue** (created in the
ctor), so `start()`/`set_enabled()`/`autoconnect_saved()` are **non-blocking** and
serialized — no caller races `bring_up()`, the LVGL thread never blocks (this
replaced ad-hoc per-call tasks that raced when a screen toggle overlapped boot
auto-connect). `set_listener()` registers the persistent listener synchronously
with **no** bring-up. Two delivery channels, like `adb`: **one-shot
completions** for `scan()`/`connect()`/`connect_saved()` (fire exactly once; plus a
`set_enabled()` `done` cb on the worker thread) AND a
**persistent `Listener::on_wifi_state(Status)`** for steady-state transitions
incl. a mid-session drop (the gap the reference NetworkManager couldn't report —
it dropped its single callback after the first success). All callbacks fire on the
backend's **event thread**, never LVGL — marshalling is the app's job; the
listener is held **weakly** (drop the shared_ptr to detach). `Status` =
`{state, ssid, ip, rssi}`; `Result` adds `Timeout` to the reference's granularity.
Credentials persist to NVS (namespace `wifi`) so `configured()`/`saved_ssid()`/
`connect_saved()` work. **Modem power-save** (`set_power_save(PowerSave)`,
`PowerSave::{Default,None}`) maps to `esp_wifi_set_ps(WIFI_PS_MIN_MODEM|WIFI_PS_NONE)`
through the backend (device) / no-op (sim); callable from any thread, a no-op until
the radio is up. `adb_app`'s connection holder asserts `None` on an **ADB-over-TCP**
link reaching Online and `Default` on its Close (a USB link leaves Wi-Fi PS untouched),
so modem sleep doesn't throttle the mirror over Wi-Fi. The mode is not persisted across
a radio off/on — the holder re-asserts it per connect. **Sim control** (`inc/wifi_sim.hpp`): the fake picks a
connect outcome from the SSID (`fail-auth`/`fail-notfound`/`fail-assoc`/`timeout`)
or an explicit `wifi::sim::set_next_connect_result()`; the sim harness exposes
`wifi-aps` / `wifi-connect-result` / `wifi-delay` / `wifi-drop` script commands and
`SIMULATOR_WIFI_CONNECT` for non-scripted runs. The **Settings → Wi-Fi** button
pushes `WiFiScreen` (a top **status card** = a Wi-Fi on/off `lv_switch` + separator
+ the connection status line (SSID when connected) + detail rows `IP Address`
(connected only) and `MAC Address` (`wifi::manager().mac_address()`, shown whenever
known), then the scan list + secured-network password modal +
connect progress/error). Both the status card and the AP list are **bordered
blocks (SettingsScreen-style — a plain `lv_obj` keeping the theme card border/
radius)**; the **body scrolls as a whole** (`LV_DIR_VER`) and the list is
`LV_SIZE_CONTENT` (sized to the AP count, no inner scroll). List rows are divided
by thin separators (`add_separator`); each row is **leading** signal glyph + SSID
+ trailing lock (secured). Signal is a Lucide Wi-Fi glyph (`wifi_icon_for_rssi`:
full/`WIFI_HIGH`/`WIFI_LOW`/`WIFI_ZERO` by RSSI, **no number**). The connected
(else saved) SSID is **pinned to the top** of the list regardless of signal (the
rest keep the scan's RSSI-desc order); the connected row's signal glyph + SSID are
both painted blue. The whole list is **hidden while Wi-Fi is off or a scan is in
flight** (`scanning_` + `update_list_visibility()`); while scanning a matching
bordered **"Searching…" block** (`scan_block_` = circular spinner + label) is shown
in its place. The
HomeScreen "Wireless (TCP/IP)" card shows a live status line plus the recent
ADB-over-TCP targets list (see the HomeScreen section). The switch drives
`wifi::manager().set_enabled()` (radio
`esp_wifi_stop`/`start`, keeping the stack); Off → `State::Off`, the screen hides
the spinner, clears the AP list, and `start_scan()`/`rebuild_list()` no-op while
off. **On also rejoins the saved network** (the worker runs the same connect path
as the boot `autoconnect_saved()`), so flipping the switch back on reconnects
without a manual tap. **The screen does NOT bring the radio up on entry** — `onEnter` only registers
the (weak) listener cheaply via `wifi::manager().set_listener()` (no bring-up).
Flipping the switch gives immediate UI feedback (switch state + spinner +
"Turning on…", and the switch is disabled for the transition), then calls the
**non-blocking** `wifi::manager().set_enabled(on, done)` — the manager's worker task
does the blocking esp_wifi work and fires `done` on the worker thread, which the
screen marshals to LVGL (`lv_async_call`) to re-enable the switch + rescan. So there
is no screen-owned task; the blocking work and the boot auto-connect both run on the
single manager worker. Because enabling **also rejoins the saved network**, the
screen does NOT scan immediately (esp_wifi can't scan mid-association — the scan
would be aborted on connect, yielding an empty list): `maybe_scan()` defers while
`State::Connecting` (`want_scan_`), and `on_wifi_state` runs the scan once the link
settles (Connected/Disconnected). The refresh button / `onAppear` go through
`maybe_scan()` too. **Boot auto-connect:** `adb_app()` calls
`wifi::manager().autoconnect_saved()` (non-blocking — enqueues to the worker) so by
the time the user opens the screen it usually opens *On* + connected (the switch
reflects `enabled()`). The screen IS the `wifi::Listener`;
verified headless via `./run.sh simverify simulator/verify/wifi.txt` (scan →
connect open → password modal → forced auth-fail error),
`simulator/verify/wifi_onoff.txt` (switch off clears the list + IP row, MAC row
stays → on rescans),
`simulator/verify/wifi_autoconnect.txt` (boot auto-connect → HomeScreen line turns
green, no tap), `simulator/verify/wifi_home_off.txt` (HomeScreen shows "Wi-Fi
off" when disabled), and `simulator/verify/wifi_list.txt` (bordered blocks,
row separators, glyph signal strength, content-sized list, list hidden while
scanning). The password
**`lv_keyboard` is anchored to the screen bottom (a child of `root_`,
`IGNORE_LAYOUT` + `ALIGN_BOTTOM_MID`), NOT inside the modal card** (where it
overflowed) — created after the scrim so taps reach it, and torn down by
`close_password_modal()` (the keyboard is a sibling of the card, so closing the
modal alone wouldn't delete it). **Keyboard gotcha:** that teardown must be
deferred via `lv_async_call` — deleting an `lv_keyboard` inside the in-flight
button/keyboard event hangs LVGL (modal_confirm's plain dialogs don't hit this).
**Next:** real
Tab5 HW bring-up (verify the C6 esp-hosted link + sdkconfig pins), then the
`embedded_adb` TCP transport.

### The JPEG decode seam (for the mirror render)

`VideoListener::on_video_strip` hands the app a whole JPEG strip to decode into the
`bsp` framebuffer. The decode is **the same call on both targets** — a
`jpeg_enh_strip_decoder_new()` handle (`ring_count = 0`, whole-frame mode) +
`jpeg_enh_decoder_process()` from `components/jpeg_decode_enhanced/` — split per
the two standard rules:

- The IDF JPEG **engine API** (`driver/jpeg_decode.h`) is Espressif's, so the
  host gets it in `simulator/idf_compat/` (libjpeg-backed). Device uses the real
  `esp_driver_jpeg` (the enhanced decoder grabs the IDF engine internally).
- The full-range conversion is **not** an Espressif feature
  (`cfg.decode.yuv_full_range = true` overrides the 2D-DMA CSC matrix registers —
  MJPEG/JFIF content is full-range, but the IDF decoder bakes in the
  *limited-range* matrix and washes the image out). So it lives in the shared,
  `ESP_PLATFORM`-branched `components/jpeg_decode_enhanced/` component (device =
  the 2D-DMA reimplementation with the matrix override; host =
  `jpeg_decode_enhanced_sim.c`, a whole-frame delegate to the libjpeg shim, which
  is already full-range). The component also offers strip-pipelined decode + a
  PPA pipeline (its Layer 2, see its README) — the mirror uses only the
  whole-frame Layer 1 API, **no PPA**. App code calls the one name and the
  simulator previews the full-range device output.
  **Colour gotcha (cost several HW cycles):** for **RGB565 output**
  on the Tab5 panel use `rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR`, **not** `RGB`.
  The panel is driven R-in-high-bits RGB565, and on the P4 the *BGR* scramble is
  what produces that packing; the *RGB* enum's 565 scramble mis-packs the 16-bit
  pixel (the 6-bit green straddles the byte boundary) so the image renders as
  rainbow noise with the structure intact. (Matches the proven
  `Tab5-Screen-Streamer` decoder, same panel.) The simulator's libjpeg shim
  (`idf_compat/jpeg_decode.c`) mirrors this — its RGB565 BGR branch packs R-high —
  so the sim previews the device-correct colours with the same decode cfg.

The whole-frame API keeps the plain IDF `jpeg_decoder_process()` output model
(tightly-packed) — no stride/offset parameter. That is deliberate: the
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
`jpeg_decode_enhanced.c`'s error path soft-resets the JPEG FSM before
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

- **Bring-up.** `ensure_connected(cb)` runs the §2.2 sequence on a private
  worker task — `exec` pkill stale agent → `Sync::push` the **embedded jar**
  (`app/agent/agent_jar.{h,c}`) → `open_shell` `app_process` → retry `Link::open`
  until the agent answers HELLO. It is the `SyncListener`/`ShellListener` for the
  push + agent stdout and the `LinkLifecycleListener` for the link.
  `cb` fires on the **LVGL thread** (like `adb_connect_async`): true once Ready,
  false on failure; already-Ready posts immediately; concurrent calls coalesce.
  The bring-up is **fail-fast** (it gates the connect UX): the HELLO retry loop
  has an overall ~8 s deadline and bails immediately when the agent's launch
  shell closes early (`on_shell_close`, app_process died — matched against the
  *current* shell_, since dropping a previous session's shell delivers a stale
  terminal close that must not fail the next bring-up).
- **Mode (the app's feature gate).** The connect flow (HomeScreen) runs
  `ensure_connected` EAGERLY right after adb Online and only then pushes the
  device screen, so every screen reads a settled `mode()`:
  `Mode::Normal` (agent up → mirror, AgentPreview, app icons) vs `Mode::Limited`
  (agent-independent features only: screencap preview, pm-list apps, no
  mirroring). Every bring-up result refines the mode (a stop_-aborted worker
  doesn't stamp Limited); adb disconnect resets it to Unknown. A mid-session
  link drop keeps the mode Normal — features lazily `ensure_connected` again on
  next use, exactly the old behavior. `agent_caps()` exposes the HELLO
  capability bits (gate `kCapAppInfo` features on it).
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

`app/` drives the connection from the LVGL UI. `HomeScreen` is a two-tone
layout: a full-bleed dark hero (title + the manually-bumped version constant in
`app/app_version.hpp`) over a light card body matching the rest of the app. The
body is a **USB card** (just the big Connect button; the Connect center stays at
(360, 420) — the verify scripts tap by coordinate), a **Wireless (TCP/IP) card**
that shows a live Wi-Fi status line plus a **recent-targets list** — `HomeScreen`
**is** a `wifi::Listener`,
re-registering itself (cheaply, via `wifi::manager().set_listener()` — no bring-up)
on `onAppear` and refreshing the line both then (`refresh_wifi_status()`) and live
on `on_wifi_state` (so the boot auto-connect completing turns the line green
without a tap) — Wi-Fi setup is opened from **Settings → Wi-Fi** (the
`WiFiScreen` scan/connect UI; see the `wifi` component section), not the
HomeScreen. The phone-IP
`Connect` row is wired: an `lv_textarea` (`host` or `host:port`, default port 5555)
+ a Connect button, with an on-screen `lv_keyboard` raised on focus (a child of
`root_`, `IGNORE_LAYOUT` + `ALIGN_BOTTOM_MID`, WiFiScreen-style; its OK confirms,
torn down via `lv_async_call` so it isn't deleted mid-event). `start_connect_tcp()`
parses host:port (`parse_host_port`), then runs the **same two-stage connect flow**
as USB (`proceed_after_connect` — adb link → eager agent bring-up → push
`ADBDeviceScreen`) via `app::adb_connect_tcp_async()`; a successful target is saved
with `app::tcp_history::add()`. Under the input row, `rebuild_tcp_history()` renders
the **recent ADB-over-TCP targets** from `app::tcp_history` (NVS-persisted
`host:port` list, most-recent-first, `kCap` entries, newline-separated under
`tab5adb/tcp_history`): a "Recent" heading + one tappable, separator-divided row
per target (lucide history glyph + `host:port`), or a "No recent connections"
placeholder when empty. Tapping a row (`select_tcp_target()`) fills the address
field and connects to that target. Rebuilt on
`build` and `onAppear` (a later connect may have added an entry). Seed the list
for simverify by writing `tab5adb/tcp_history` into the gitignored `nvs_data.json`
(newline-joined `host:port`, hex+trailing-NUL — the sim NVS stores values as hex);
`simulator/verify/tcp_history.txt` then renders it + taps a row. A bottom
row of three nav cards: **About** (placeholder) and **Settings** and
**Files** at (360, 1170), which pushes `SDFileBrowserScreen`
directly — the local browser (and its previews / APK info) needs no adb
connection (`simulator/verify/home_sd.txt`). The hero fonts pulled
montserrat 18 + 48 into the device sdkconfig (the sim's `lv_conf.h` enables all
sizes). Tapping Connect opens a **connect-progress modal** (spinner + message
over a `modal_open` scrim, `HomeScreen::open_progress`/`set_progress`/`close_progress`):
the scrim covers the cards for the whole connect flow so the user can't navigate
away or kick off TCP/IP mid-connect (the reason the old inline `status_label_`
status slot was replaced — and what unblocks the coming wireless path). The flow
calls `app::adb_connect_async()`, then — on adb
Online — chains `app::agent_client().ensure_connected()` (updates the modal to
"Starting agent on the phone...") and `close_progress()` + pushes `ADBDeviceScreen`
only once the agent mode settled
(success and failure both proceed; Limited mode just hides the agent-backed
features). An adb-stage failure closes the modal and shows a
`modal_message("Connection failed", …)` error instead. The holder is a small app-global in
`app/adb_app.cpp` that owns the single `std::shared_ptr<adb::Client>` (it must
outlive the transient screens) and implements `adb::ClientListener`. The `Client`
owns the connection lifecycle + reader task; the holder's only job is to marshal
the reader-thread `on_state` callbacks to the LVGL thread with `lv_async_call`
(marshalling is the app's job — the library never touches LVGL). On reaching
Online it pushes `ADBDeviceScreen`. The app pulls in no
protocol/transport details, only the `adb` component's typed surface. On `Closed`
the holder also calls `app::agent_client().on_adb_disconnected()` so the
tab5adb-agent connection is torn down with the adb link. **Transport seam:** the
holder tracks USB vs TCP (`g_connection_is_usb`) and exposes it generically via
**`app::connection_transport()`** (`-> Transport::{Usb,Tcp}`) + the
`app::connection_is_tcp()` convenience (`adb_app.hpp`, callable from any thread) —
the general-purpose hook for any feature that must branch on the link kind (not just
the VBUS/Wi-Fi-PS policy that motivated the original flag). First consumer: the
mirror tunes JPEG params down for the slower TCP/Wi-Fi link (see
`ADBMirroringScreen`). `ADBDeviceScreen`'s
**Disconnect** tool button (confirm modal) calls `app::adb_disconnect()` —
`g_client->close()` (which fires `on_state(Closed)` → the agent teardown above) +
drops the holder's `shared_ptr` so `adb_client()` is null — then
`screen_manager.load(HomeScreen)` to unwind back to the start
(`simulator/verify/disconnect.txt`, verified on a real Android device).

`ADBDeviceScreen`'s **summary header** is a tappable card: device name
(`settings get global device_name`, falls back to the banner model), a
"model • Android ver • marketed storage" sub-line, and battery (+%) / Wi-Fi /
cellular status icons; tap → **`ADBDeviceInfoScreen`**. It renders immediately
from the CNXN banner, then **one chained exec** (`---SEP---`-separated:
device_name / version / `dumpsys battery` / `df -k /data` / `cmd wifi status` /
`gsm.sim.state` / a `dumpsys telephony.registry` grep) fills the live fields —
parsed **on the reader thread**, applied in one `lv_async_call`; `onAppear`
fetches + starts a **10 s `lv_timer`** re-fetch (killed in `onDisappear`,
in-flight flag prevents overlap). Parsing lives in **`app/device_info.{hpp,cpp}`**
(`app::devinfo`) — pure string parsers/formatters (battery / wifi / cellular /
df / meminfo / `/proc/stat` deltas / getprop), no LVGL/adb deps, **host-tested**
with verbatim Pixel 10 fixtures via `nix develop -c app/test/run.sh` (the app's
own test runner; `app/CMakeLists.txt` excludes `app/test/` from the source glob —
those have their own `main()`). Icon/color mapping (lucide glyphs) is the
UI-side companion `app/device_icons.hpp`. Parser contract: **a field that fails
to parse is hidden/grey, never an error** (formats vary by vendor/version —
e.g. a SIM-less device still reports LTE `level=3`, so the cellular icon gates on
`gsm.sim.state` containing READY/LOADED, and `cmd wifi status`'s first `RSSI:`
is the active one, before the MLO link list). Within the header card the child
containers need `LV_OBJ_FLAG_CLICKABLE` *removed* or they swallow the tap
(plain `lv_obj`s are clickable by default and the click doesn't bubble).

**`ADBDeviceInfoScreen`** (`app/adb_device_info_screen.*`) — device detail:
one chained exec (full `getprop` + meminfo grep + df + battery + wifi +
telephony grep + `/proc/version` + `wm size`/`density` + max cpufreq/core count
+ `/proc/uptime`) parsed on the reader thread into a `DeviceInfoData`, then one
marshalled rebuild: six **featured cards** in a 2-column wrap (SoC `ro.soc.*` /
Memory marketed+available / Storage used-total with an `lv_bar` / Battery
level•status•health•temp / Network wifi state+RSSI+IP & cellular state / System
Android ver•SDK•patch•build•kernel), a **Performance Metrics** button (→
`ADBMetricsScreen`), and a key-value list of the miscellaneous fields
(manufacturer…fingerprint…timezone; empty values skipped). Spinner until the
exec lands (AppDetail pattern).

**`ADBMetricsScreen`** (`app/adb_metrics_screen.*`) — live CPU/memory: **one
streaming shell** runs a device-side loop (`while true; do echo @@@; grep ^cpu
/proc/stat; …; sleep 1; done`) instead of an exec per tick — the device's
`sleep 1` is the tick, no per-sample fork/stream-open. Logcat-pattern threading
(`on_shell_data` → capped FIFO + one coalesced `lv_async_call`; frames split on
the `@@@` marker on the LVGL thread, **latest complete frame wins**). CPU % =
delta of consecutive `/proc/stat` samples (`devinfo::cpu_usage`; `top` output is
too vendor-dependent): total % + a 60-point `lv_chart` (`lv_chart_set_axis_range`
/ `set_all_values` — the LVGL ≥9.5 names, both targets) + per-core `lv_bar` rows
built on the first sample, memory used/total + %-chart, load average + battery
temp rows. `onDisappear` closes the shell (USB traffic stops; `expected_closes_`
distinguishes our close from a dying stream), `onAppear` reopens with a fresh
baseline. All three verified headless against a real Android device:
`./run.sh simverify simulator/verify/device_summary.txt` / `device_info.txt` /
`metrics.txt` (the last exercises the close-on-leave + reopen-on-reentry cycle).

`ADBDeviceScreen`'s **Shell** button pushes **`ADBShellScreen`**
(`app/adb_shell_screen.*`) — a real VT terminal over `Client::open_shell()`,
light-mode (FileManager-style 120px nav bar) with a **direct-typing UX**: there
is no input line — the on-screen keys send bytes straight to the PTY and the
PTY's echo is the feedback. Three pieces:
- **`components/term_emu`** — a pure-C++ (no LVGL/adb) VT100/xterm-subset
  terminal emulator: bytes in, 72×42 `Cell` grid out. Paul-Williams-style FSM
  (`src/vt_parser.cpp`) + grid ops (`src/term_emu.cpp`): cursor/erase/
  insert-delete CSI set, DECSTBM regions, SGR (16 + bright + 38/48;5 256-color,
  default fg/bg as attr bits), DEC modes (DECCKM, DECAWM with deferred wrap,
  ?25 cursor, ?1049 alt screen), DSR/DA responses via a responder callback,
  OSC/DCS discarded, `ESC ( 0` DEC graphics → ASCII approximations, UTF-8
  decoded to '?' placeholder cells (wide CJK = 2 cells) so the cursor column
  tracks the device's wcwidth. ~1000-line PSRAM scrollback
  (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`, row-pointer rotation on scroll).
  Single-threaded by design — feed it on the LVGL thread only. Host unit test:
  `nix develop -c components/term_emu/test/run.sh` (no phone; includes a
  chunk-split fuzz that catches FSM state lost across feed() boundaries).
- **`app/terminal/term_view`** — the renderer: a plain `lv_obj` +
  `LV_EVENT_DRAW_MAIN` drawing bg-runs (`lv_draw_fill`) and (fg,underline)-
  grouped text runs (`lv_draw_label`, **`text_local=1` mandatory** — LVGL
  renders draw tasks after the event returns, a stack buffer would dangle).
  Hack 16px cells (`R.font.hack_16`, **10×17px** — LVGL rounds the fmt_txt
  advance per glyph, `(154+8)>>4`, so the grid pitch is 10 not 154/16; the
  vendored font's `.fallback` is stripped to NULL, montserrat metrics would
  break the grid and the emulator never emits non-ASCII anyway). Dirty rows →
  `lv_obj_invalidate_area` (absolute coords); draw walks only clip-intersecting
  rows. Light 256-color palette (Tango-ish 16 base, 7/15 darkened to grays so
  "white" text survives the white bg); bold→bright, dim→mix toward bg, reverse
  swaps. Block cursor with a 530ms blink timer (invalidates one cell). Swipe
  down = scrollback (LV_EVENT_PRESSING vect accumulation; viewport stays pinned
  while output arrives by growing the offset with scrollback_used()).
- **`app/terminal/term_keyboard`** — a raw `lv_buttonmatrix` (NOT `lv_keyboard`
  — that's textarea-coupled), 3 static maps (base/Shift/Sym), Esc/Tab/arrows/
  Home..Del, sticky one-shot Ctrl (`c & 0x1f`), Shift one-shot, Sym latched.
  Arrows/Home/End honour DECCKM (CSI vs SS3) via a query into the emulator.
  Keys auto-repeat (press-time `VALUE_CHANGED` + `LONG_PRESSED_REPEAT`); **the
  modifiers are deliberately NOT `CHECKABLE`** — lv_buttonmatrix toggles
  CHECKED on *release*, after the press-time VALUE_CHANGED the handler keys
  off, so the armed state is tracked in the class and CHECKED is set/cleared
  manually for the visual.
The screen **is** the `adb::ShellListener`: `on_shell_data` (reader thread)
appends to the capped (~256KB) `pending_out_` FIFO and coalesces one
`lv_async_call` flush that runs `TermEmu::feed()` + `TermView::refresh()` on
the LVGL thread (on overflow the flush feeds CAN to re-ground the parser, then
an `[output dropped]` marker). Key bytes go `shell_->write()` directly
(non-blocking; QueueFull = key dropped) after `snap_to_live()`. v1 `shell:` has
no window-size/TERM channel, so `build()` bootstraps the PTY in-band:
`stty rows 42 columns 72; export TERM=xterm-256color; clear` as the first write
(pre-open writes queue until the stream opens; the echo is wiped by the
`clear`). Teardown is the standard pattern: `onExit()` → `shell_->close()`,
weak listener, `self = shared_from_this()` + `Screen::exited()` guards in every
marshalled lambda. Verified headless against a real Android device
(`./run.sh simverify simulator/verify/terminal.txt`): prompt, `ls --color`,
`top` (full-screen redraw + reverse video, q back to prompt), getprop +
swipe-scrollback, Ctrl+C (`^C` + exit code 130). Device-side flash-and-check
(scroll-burst redraw cost) is still pending.

`ADBDeviceScreen`'s **File Manager** button pushes **`ADBFileManagerScreen`**
(`app/adb_file_manager_screen.*`) — a virtual root that lists the available
storages (Android `/sdcard`, `/`, and the Tab5 SD card) as cards. Tapping a
storage pushes **`ADBFileBrowserScreen`** (`app/adb_file_browser_screen.*`), the
Android file browser over `Client::open_sync()` + `Sync::list()`. The
browser **is** the `adb::SyncListener`: it opens a `sync:` session, lists the
directory (folders first, then case-insensitive by name; `.`/`..` filtered) and
tapping a folder descends into it; **tapping a plain file pushes its preview
screen** (`app::make_file_preview`, see the file-preview section below). A
`directory_stack_` holds the path history —
each level caches its entries — and the nav **Back** button goes up a level, or
pops the screen at the stack root. Same two threading concerns as the shell
screen, but note the Sync refinement: `list()` completions fire on the **Sync
worker thread** (not the reader thread), so they marshal to LVGL with
`lv_async_call`; `onExit()` just does `close()` (the session holds the listener
as a `weak_ptr` — the screen passes a `shared_ptr` aliasing `shared_from_this()`,
so no `detach()`); marshalled lambdas capture `self = shared_from_this()` and
skip on `Screen::exited()`. One extra guard: a `nav_gen_` counter (LVGL-thread
only) bumped on every navigation drops **stale list completions** when the user
taps faster than the device responds. A second mode, **pick-dir**
(`ADBFileBrowserScreen::PickDir{label, on_pick}` — the copy-destination picker):
files are inert/greyed, and a nav-bar confirm button (e.g. "Copy Here") pops the
screen then calls `on_pick(current dir)` (copy to locals before the pop — pop
frees the lambda's storage); Back at the root pops without the callback (=
cancel).

The FileManager's **SD Card** card pushes **`SDFileBrowserScreen`**
(`app/sd_file_browser_screen.*`) — the *local* (Tab5 SD card) counterpart of
the ADB browser: `bsp_sd_mount("/sd")` on demand at build, then synchronous
POSIX `opendir`/`readdir` scans **on the LVGL thread** (local FS is fast; no
listener/marshalling machinery), same list UI/sort, dotfiles skipped, errors
("SD card not found." / unreadable dir) shown in-list with Refresh re-mounting.
Three modes: **browse** (default; tapping a plain file pushes its preview
screen), **pick**
(`SDFileBrowserScreen::Pick{ext, on_pick}` — only files matching the
case-insensitive extension are tappable, the rest are greyed; tapping one
**pops the screen first**, then calls `on_pick(path)` on the LVGL thread with
the caller's screen back on top — copy the callback/path to locals before the
pop since pop frees the lambda's storage), and **pick-dir**
(`SDFileBrowserScreen::PickDir{label, on_pick}` — same contract as the ADB
browser's pick-dir mode above). Also reachable without adb from the
HomeScreen's SD Card button. Verified headless:
`simulator/verify/sdcard.txt` (browse vs `simulator/sdcard/`),
`home_sd.txt` (the adb-less entry). On the host, `sd_redirect.c` forwards
`open/fopen/opendir/stat` **plus `rename`/`unlink`** (the pull-to-SD `.part` →
final rename needs them) to the redirected path.

**File previews + Android⇄SD copy.** Tapping a plain file in either browser
pushes a per-type **preview screen** from the extension-keyed registry in
**`app/file_preview.{hpp,cpp}`**: `app::FileRef{where: SD|Android, path, size,
mtime}` (the Android side passes the `DirEntry` metadata; the SD side stats
locally) → `make_file_preview(ref)` — never null, unmatched extensions get the
**generic preview** (info card: location/size/mtime/path + the copy actions).
The same file also exports the shared preview building blocks
(`preview_chrome`/`preview_header`/`preview_info_row`/`preview_action`) so
per-type screens look alike. Copy lives in **`app/file_transfer.{hpp,cpp}`**,
deliberately **screen-agnostic** (a later long-press context menu can drive the
same calls from a `FileRef` without any preview screen): `pull_to_sd` /
`push_to_android` / `install_apk` each return a `shared_ptr<TransferJob>`
(caller stores it and calls `abort()` in `onExit()`). A job owns its **own
`Sync` session** (a pull abort closes the session — RECV has no wire cancel —
so it must never borrow the browser's), the overwrite-confirm step (local
`stat()` for pull; `Sync::stat` for push, incl. an exists-but-directory error),
and an install-flow-style progress modal (Cancel → atomic abort; 200 ms
`lv_timer` renders an atomic byte counter). Pull writes worker-thread-side to
`<name>.part` and renames on success (failure/abort unlinks). The job watches
its parent root's and card's **`LV_EVENT_DELETE`** (weak self) so a screen
teardown mid-transfer nulls the UI pointers and the transfer ends quietly —
this is what required the `lvgl++` fix below. Flow: preview action ("Copy to SD
Card" / "Copy to Android", the latter greyed when adb isn't Online) → the
*other* browser in pick-dir mode → "Copy Here" → job. Verified E2E headless
against a real Android device: `simulator/verify/copy_push.txt` (SD→Android; check the file
on the phone, then `adb shell rm /sdcard/test.txt`) and `copy_pull.txt`
(Android→SD into `simulator/sdcard/folder/`; needs the prep `adb push`
documented in the script, delete the copied file after), `file_preview.txt`
(generic preview, no phone).

**`lvgl++` gotcha (cost the copy-flow bring-up):** `lv_obj_add_event_fn` also
registers a cleanup callback that deletes the heap `std::function` on
`LV_EVENT_DELETE`. It used to register the cleanup *first*, so an event handler
*filtered on* `LV_EVENT_DELETE` ran after its function was deleted
(`bad_function_call` at screen teardown). Callbacks fire in registration order,
so the handler is now registered before the cleanup — DELETE-filtered handlers
(the transfer-job watches) run exactly once, then are freed.

**APK preview** — the first registry entry (`.apk` on the **SD** side; an
Android-side .apk stays generic — the parser reads local files, copy it to SD
first). **`ApkPreviewScreen`** (`app/apk_preview_screen.*`) shows the manifest
metadata parsed **locally, no device needed** by **`app/apk_info.{hpp,cpp}`**
(`app::apkinfo::parse`): zip EOCD → central directory → `AndroidManifest.xml`
(stored or raw-deflate via zlib, already linked on both targets) → binary AXML
(UTF-8/UTF-16 string pools, `manifest`/`uses-sdk`/`application` attributes →
package / versionName / versionCode / minSdk / targetSdk / label). Pure parser
(no LVGL/adb), `device_info`-style **host-tested**: `TEST=test_apk_info nix
develop -c app/test/run.sh` (fixtures = `simulator/sdcard/testapp.apk` +
`dummy.apk` rejected; the runner passes the repo root as argv[1] and links
`-lz`). Parser contract as ever: a missing field is hidden, never an error; a
literal `android:label` is shown (header title), a resource reference stays
empty (resources.arsc is out of scope). The **Install** action (greyed
"(not connected)" unless adb is Online) confirm-modals then runs the shared
`app::install_apk`. Verified headless: `simulator/verify/apk_preview.txt`
(offline parse, no phone) and `apk_preview_install.txt` (real install on the
real Android device; `adb uninstall com.tab5adb.testapp` after).

`ADBDeviceScreen`'s **Apps** button pushes **`ADBAppManagerScreen`**
(`app/adb_app_manager_screen.*`) — the installed-app manager. Listing has two
paths picked per refresh: in **Normal mode** (an APPINFO-capable agent link) one
`Link::request(GET_APP_LIST)` returns label-sorted `AppEntry{pkg, label}` +
system/disabled flags, **parsed on the adb reader thread** (the LVGL thread just
swaps the vectors in); otherwise — Limited mode, a dropped link, or a refused
request — the original **one exec round trip** fallback runs (`pm list packages
-3/-s/-d` with `---SEP---`, package names only, same `load_gen_`). In Normal
mode the visible rows also **lazily fetch launcher icons** (`GET_APP_ICON`, raw
ARGB8888 at 56px straight into an `lv_image_dsc_t` over a PSRAM buffer): each
`update_rows` pass runs `pump_icons()` — at most 4 requests in flight (a fast
fling doesn't queue every row it passed), a per-screen `icons_` cache (std::map,
node-stable dsc addresses; capped at 256, beyond which rows keep the package
glyph) and an `icon_pending_` set, all LVGL-thread-only (the request completion
copies the pixels on the reader thread, then marshals). The list is **recycled,
RecyclerView-style** (a
per-package LVGL build was visibly slow on device even for the User set): a
fixed pool of row widgets (`ensure_pool`, viewport/81px + 3, created once) is
**rebound** to the visible index window on `LV_EVENT_SCROLL`
(`update_rows`/`bind_row` — set y/labels/icon-vs-glyph/disabled-tag, no object
churn), an
invisible `extent_` child spans `count*81` to define the scroll range, the
row's click handler reads its bound `data_idx` at tap time, and the separator
is the row's own bottom border. While a listing is in flight the list shows a
**spinner** (FileBrowser-style); the scroll position survives an `onAppear()`
re-list (clamped if the list shrank) and resets on filter switch. A **User /
System** filter toggle picks the rendered set (User default; disabled packages
grey with a "disabled" tag); rows push **`ADBAppDetailScreen`**. `onAppear()`
re-lists (so returning from detail/install refreshes); a `load_gen_` counter
drops stale completions on both paths.
**`ADBAppDetailScreen`** (`app/adb_app_detail_screen.*`) shows
version/installed/updated/path/status parsed out of one `dumpsys package
<pkg>` (`<key>=` to end-of-line; install times contain spaces) and the
actions: **Launch** (`monkey -p <pkg> -c ...LAUNCHER 1`; failure modal when
"Events injected" is missing), **Force stop** (`am force-stop`), **Clear
data** (`pm clear`, confirm), **Uninstall** (user apps; `pm uninstall`,
confirm, pops on Success), **Disable** (`pm disable-user --user 0`, system
apps, confirm) / **Enable** (`pm enable`, offered whenever disabled). Confirm/
result dialogs are the shared **`app/modal.{hpp,cpp}`** helpers
(`modal_confirm`/`modal_message`/`modal_open` — scrim with
`LV_OBJ_FLAG_IGNORE_LAYOUT` pinned over the whole screen root + centered card;
dialogs die with the screen). All exec completions marshal with
`lv_async_call` + `self`/`exited()` guards.

The app manager's nav **Install** button runs the **APK install flow**:
`SDFileBrowserScreen` in pick mode (`.apk`) → size-confirm modal → the shared
**`app::install_apk`** transfer job (file_transfer: `Sync::push` to
`/data/local/tmp/tab5adb_install.apk` with the progress dialog → `pm install
-r` → `rm -f` the temp + result modal; the push `SyncSource` runs on the
**Sync worker thread** and reads the file per the SD rule — 16 KB `read()`
chunks into a `MALLOC_CAP_CACHE_ALIGNED` buffer — bumping an atomic byte
counter a 200 ms `lv_timer` renders). The screen holds the returned
`TransferJob` and `abort()`s it in `onExit()`; `on_done(true)` re-lists. The
same flow backs the APK preview's Install. Verified
E2E headless against a real Android device (`simulator/verify/apps.txt`, `apps_scroll.txt`
— the recycler under a long drag (note: `down`/`move` only write the touch
snapshot, so drag scripts need `wait`s between steps to be sampled as a drag) —
`appdetail.txt`, `apk_install.txt` — the last really installs `simulator/sdcard/testapp.apk`'s
`com.tab5adb.testapp` on the phone; clean up with `adb uninstall
com.tab5adb.testapp`).

`ADBDeviceScreen`'s **Logcat** button pushes **`ADBLogcatScreen`**
(`app/adb_logcat_screen.*`) — a live `logcat` viewer over a streaming
`Client::open_shell(listener, "logcat -v threadtime -T 500")` session. Lines
live in a **PSRAM `LogRing`** (2 MB byte pool + 16 K line descriptors, oldest
evicted first; a line never splits across the pool end — the tail is abandoned
on wrap, and lines *ahead of the write offset are always the oldest*, which is
what keeps head-order eviction == offset-order overwrite). Every line gets a
monotonic `seq`, so the filtered view (a `deque<seq>`) and scroll anchoring
survive eviction. Threading is the ShellScreen pattern: `on_shell_data` (reader
thread) appends to the capped FIFO + coalesces one `lv_async_call`; the flush
splits/parses (threadtime level + timestamp) and appends to the ring **on the
LVGL thread only** (no ring lock). The list is the AppManager-style **recycled
row pool** (24 px hack_16 rows, extent child, rebind on scroll) and rendering is
**throttled**: appends only set `dirty_`, a 100 ms `lv_timer` does the
extent/scroll/rebind pass; at the bottom the view follows the tail, scrolling up
detaches (floating jump-to-live button returns), and front-evicted rows pull
`scroll_y` back so a scrolled-back view stays anchored. Filtering is
client-side (the buffer always keeps everything): min-level chips (V/D/I/W/E,
selected chip wears the level color) + a substring `lv_textarea` whose
`lv_keyboard` applies on READY/DEFOCUSED — two LVGL gotchas baked in:
the keyboard widget pre-sets `ALIGN_BOTTOM_MID`, so `lv_obj_set_pos` *offsets
from the bottom* (re-`align` it instead), and it drops `CLICK_FOCUSABLE`, so
typing never updates the indev's last-pressed and re-tapping the still-focused
textarea sends **no FOCUSED** — hook `LV_EVENT_CLICKED` too. Tapping a (clipped)
row shows the full line in a modal. **Pause** closes the shell (USB traffic
stops too; `expected_closes_` tells our own closes from a dying logcat);
**resume** reopens with `-T '<last timestamp>'` so the paused span backfills
(same-ms lines may duplicate; logcat re-emits its `--------- beginning of ...`
buffer headers mid-stream). **Save** snapshots the ring into a PSRAM buffer on
the LVGL thread and a one-shot FreeRTOS task writes `/sd/logcat_NNN.txt`
(no RTC → sequence number, not a date; the job owns the buffer, InstallJob
style). Verified E2E headless against a real Android device
(`./run.sh simverify simulator/verify/logcat.txt` — live tail, both filters via
on-screen-keyboard taps, pause/resume backfill, scrollback + jump, modal, and a
real save into `simulator/sdcard/`; delete the `logcat_*.txt` it leaves there).

`ADBDeviceScreen`'s **screen preview** (the tappable image column next to the
tools) is mode-switched in `startPreview()`: **Normal mode** uses
**`AgentPreview`** (`app/agent_preview.*`) — a small live mirror over the agent
link: `start_mirror(360×860 box, scale=kScaleAspect, max_fps=10, jpeg_quality=60,
split_count=1)`, so the **agent** sizes the stream to the source's natural
aspect (fixed 360 width, height follows the phone — the ScreencapPreview
behavior) and sends each frame as ONE whole JPEG (`split_count=1` — no strip
banding for a small frame, which is what frees the size from 16px alignment);
the `MirrorInfo.out_*` dims size the frames. It
is the mirror screen's receive/decode split scaled down to LVGL: reader thread
copies the frame JPEG into slots (3, latest-frame-wins), a low-prio Core-1
decode task runs a `jpeg_decode_enhanced` whole-frame decode into a
double-buffered RGB565 `lv_image`, one `lv_async_call` flips per frame
(`present_pending_` keeps the decoder off a buffer whose flip is still queued).
Two decoder rules baked in (the P4 HW constraints, §5.2): the buffers are
**64-byte aligned** (`heap_caps_aligned_alloc` — heap_caps_calloc tripped the
device's cache-line check) and sized for the **MCU-padded** box, and the
decoded raster is read at the padded stride (`jpeg_enh_frame_info_t.pic_w`,
368 on device vs 360 on the host's libjpeg shim) via the lv_image dsc stride
while showing the real 360×h frame.
Natural-orientation lock means a rotated phone keeps its portrait frame here
(screencap followed the logical rotation — accepted difference). **Limited
mode** keeps the old **`ScreencapPreview`** (`exec:screencap -p`, 360×860 box);
a Normal-mode `ensure_connected` failure degrades to it in place. `stop()`
sends MIRROR_STOP but keeps the link; previews are created/stopped on
appear/disappear, so pushing the mirroring screen stops the preview stream
first (and the agent tolerates the reconfigure either way). **Tapping the
preview** is the mirroring entry: Normal → push `ADBMirroringScreen`; Limited →
a `modal_message` explaining the agent couldn't be started (the old Mirroring
tool button is gone). Directly **under** the preview (in the same 360-wide
column, `createNavBar`) sits a `[ Back | Home | Recents | Power ]` row, rendered
as one bordered bar with thin separators between the four flat icon buttons:
Back/Home/Recents `flex_grow` to share the width evenly, Power is a fixed narrow
button pinned right. Unlike the mirror overlay's low-latency INPUT channel these
go over plain **`app::adb_client()->exec("input keyevent <code>")`** (KEYCODE
Back=4/Home=3/AppSwitch=187/Power=26) — agent-independent, so they work in
**both Normal and Limited mode**; the small `input` fork latency is fine for
discrete taps. Gesture-nav devices still get the 3 nav keys (the KEYCODEs work
regardless of nav mode), same as the mirror overlay. Verified on a real Android device via
`simulator/verify/device_nav.txt` (Recents tap → the phone's overview shows in
the live preview, Home → back to the launcher).

Tapping the preview (Normal mode) pushes **`ADBMirroringScreen`**
(`app/adb_mirroring_screen.*`) — the live screen-mirror viewer over `agent_link`.
The screen **is** the `agent_link::VideoListener`; the agent lifecycle (jar push +
`app_process` launch + HELLO) is **not** the screen's job — it belongs to the
app-global **`app::AgentClient`** (see the AgentClient section). `onEnter()` checks
`app::agent_client().ready()`: if the agent is already live it starts immediately
(no wait); otherwise it shows a centered **"Connecting…"** LVGL label and calls
`ensure_connected(cb)` (cb on the LVGL thread). On success `start_mirror_ui()` drops
the label, enters DM overlay mode, registers the screen as the link's video listener
(`link()->set_video_listener(self)`), and calls `link()->start_mirror()` (720×1280
fit, video). The `MirrorConfig` is built by **`mirror_config_for(mode)`**, which also
**branches on `app::connection_is_tcp()`**: over USB it keeps the agent defaults
(quality 80 / uncapped fps / 4 strips), but over the slower, higher-latency TCP/Wi-Fi
link it drops to **`jpeg_quality=40`, `max_fps=15`, `split_count=16`** (smaller
per-strip JPEGs = less head-of-line latency per frame; 1280/16 = 80px strips, still
16-aligned). `onExit()` calls `link()->stop_mirror()` + `set_video_listener({})` —
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
  the HW-JPEG input DMA reads PSRAM directly and the enhanced decoder syncs the
  input cache `UNALIGNED`, so concatenated strips need no DMA bounce buffer and no
  per-strip alignment — **one fewer copy** than the old per-strip DMA input) plus a
  per-strip `{y,h,off,len}` descriptor; on `frame_end` it publishes the finished
  frame and returns, so the link acks immediately and the USB stream keeps flowing.
- **consumer = a private decode task** (`decode_loop`): drains finished frames,
  HW-JPEG-decodes each strip through the `jpeg_decode_enhanced` seam **straight
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
**DispMode cycles the display mode Fit → Fill → Adapt → Fit** (the icon is fixed —
the active mode is read off the image): **Fit** = agent `scale=fit` (letterbox),
**Fill** = `scale=fill` (cover + crop), **Adapt** = `wm size` the source to the
panel aspect (long:short = `PANEL_H:PANEL_W` = 16:9, keeping the device's short
side; `compute_adapt_size` parses `wm size`'s *Physical size*) so plain `scale=fit`
then fills with no letterbox *or* crop. Each tap calls `apply_disp_mode` →
`restart_mirror` = **one fresh `start_mirror(cfg)`**: the agent **reconfigures the
live stream in place** (it breaks `streamVideo` when a new `MIRROR_START` sets
`pendingStart`, then the session loop restarts with the new params) — no
`stop_mirror` needed (a stop+start race could hang the agent), and the video
listener / decode pipeline stay up. Entering Adapt runs `wm size <W>x<H>` and
leaving it (or `onExit` while in Adapt) runs `wm size reset` to restore the device
resolution; both chain `restart_mirror` over the `app::adb_client()->exec()`
completion marshalled back to LVGL. **DispMode availability is gated on the source's
`wm size` at mirror start** (`query_disp_mode_availability`, one `wm size` query whose
result sets two LVGL-thread flags; the effective resolution = the `Override size:` if
present, else `Physical size:`): if it is **already panel-aspect** (9:16/16:9,
`is_panel_aspect`) the button is **hidden** (`dispmode_show_` → `build_overlay_buttons`
skips it; fit==fill==adapt anyway); else if a **non-default override** is set
(`Override size:` ≠ `Physical size:`) **Adapt is dropped** from the cycle
(`adapt_allowed_` → tap does `(disp+1)%2`, Fit↔Fill only) so it never clobbers the
user's `wm size`. The flags load async (the overlay is built shown/full-cycle first;
only hiding forces a re-`apply_overlay`, the cycle length is read live). (All four
states + the in-place reconfigure + the `wm size` restore + the
hidden/adapt-disabled gating verified on a real Android device via
`simulator/verify/mirror_dispmode.txt` / `mirror_hidden.txt` / `mirror_noadapt.txt`.)
**Touch passthrough (§4.7):** when
OpMode is on, the screen's `on_touch` injects touches over the mirror to the source
as per-pointer MotionEvents via `agent_client().link()->inject_touch(action,
pointer_id, x, y)` (Tab5 **panel coords**; the agent inverts the mirror geometry).
`on_touch` keeps a small id-keyed table (`pass_[]`, guarded by `pass_mtx_`) and
diffs each touch-task snapshot into per-pointer DOWN/MOVE/UP, keying off the BSP
`bsp_touch_point_t.id` (the controller track id) so multi-touch needs no
id synthesis; each new pointer is classified once — **Pass** (injected), **Reveal**
(a corner-swipe candidate), or **Ignore** (over a visible strip / passthrough off) —
and keeps that role until it lifts. **Touches are sent BATCHED** to cut the
per-event transaction cost that drags down the link (small high-frequency touch
packets contend with the inbound video's ADB flow control — see the agent_link
section): each `on_touch` sample appends its per-pointer transitions to `tx_batch_`
(guarded by `pass_mtx_`), flushed as ONE `inject_touch_batch` frame when the link
is idle (`link()->tx_pending_bytes() == 0`) or a DOWN/UP edge needs to go now. On
USB the link is idle at every ~60 Hz sample → flush every sample = one transition
per frame (**unchanged from per-event sending — no batching, no added latency**);
on the slower TCP/Wi-Fi link the MOVEs that arrive mid-round-trip accumulate and
ship together the instant it frees (~one frame per RTT, no points dropped, no
deliberate delay — the samples were going to wait for the link anyway). DOWN/UP
force a flush; a MOVE past `kBatchMax` (24) evicts the oldest queued MOVE so a
stalled link can't grow the frame unbounded (the batch only holds MOVEs between
flushes). `release_all_pointers()` flushes the pending batch + UPs. `onExit`/dtor + turning OpMode off call
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
  `move <x> <y>`, `up`, `quit`, plus the Wi-Fi fake-backend controls
  `wifi-aps <ssid:rssi:secured,...>` / `wifi-connect-result <Result>` /
  `wifi-delay <ms>` / `wifi-drop` (drive `wifi::sim::*`). `settle` pumps `lv_timer_handler` until no
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
    **full-range**, which is what the device's `jpeg_decode_enhanced` produces
    with `yuv_full_range` (see that component). pkg `libjpeg` links to the
    `simulator` exe in `simulator/CMakeLists.txt`.
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
