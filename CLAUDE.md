# Tab5-ADB — Notes for Claude

ADB (Android Debug Bridge) client on M5Stack Tab5 (ESP32-P4). Scaffolded from
the `Tab5-UVC-Display` project (build system, BSP, LVGL infra) with a host
simulator modeled on `NameCardKnot`.

> **Keep this file current.** When you change the build flow, the simulator
> architecture, the `pf_port` surface, add a target, or hit a non-obvious gotcha
> worth remembering, update CLAUDE.md in the same change. It is the handoff to
> the next session.

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

### Host simulator — `simulator/` (SDL2 + LVGL + FreeRTOS POSIX port)

Runs `app/` on the desktop so UI / app logic can be developed without a board.

```sh
./run.sh                # == ./run.sh simulator
# expands to:
# nix develop -c sh -c 'cmake --fresh -S simulator -B build -G Ninja && cmake --build build && ./build/simulator'
```

`run.sh simulator` only re-runs `cmake --fresh` when `build/` is missing; delete
`build/` to force a clean reconfigure (e.g. after editing `simulator/CMakeLists.txt`).

## Architecture

`app/`, `components/`, and `idf-components/` are **shared** between device and
simulator. The seam is the `pf_port` namespace (`idf-components/main/platform_port.hpp`):

- Device implementation: `idf-components/main/main.cpp` (BSP + esp_lvgl_port).
- Simulator implementation: `simulator/tab5-bsp_simulator/platform_port_sim.cpp`
  (SDL window/texture for display, mouse for touch). Same header, so `app/`
  compiles unchanged.

`app_main()` lives in each platform's port file (device `main.cpp`; simulator
`platform_port_sim.cpp`) and just calls `adb_app()` in `app/adb_app.cpp`, which
sets up the LVGL display on the two `pf_port` frame buffers and pushes the first
screen. Panel is 720×1280 portrait RGB565 (`PANEL_W`/`PANEL_H` in `app/adb_app.hpp`).

Layout:
- `idf-components/main/` — entry point + `pf_port` (device), `nvs.hpp`.
- `idf-components/m5tab5-bsp/` — board support (display/touch/audio/power). Copied
  wholesale from Tab5-UVC-Display.
- `components/lvgl++/` — C++ helpers (`lv_async_call`, `lv_obj_add_event_fn`).
- `components/screen_manager/` — `Screen` base + screen stack (`screen_manager`).
- `app/` — screens and app logic.
- `simulator/` — host build (see below).

## Simulator details (FreeRTOS POSIX port)

The simulator links the **FreeRTOS-Kernel GCC/Posix port** so app code can use
real FreeRTOS primitives (`xTaskCreate`, `vTaskDelay`, queues, semaphores) on the
host. Verified working end-to-end (task entry → `vTaskDelay` ticks → `vTaskDelete`).

Hard constraints — violate these and it hangs/crashes:

1. **SDL/LVGL own the main thread.** `simulator/src/main.cpp` runs the LVGL/SDL
   loop on the main thread and starts `vTaskStartScheduler()` on a *background
   pthread*. SDL must be driven from the main thread on macOS, so the scheduler
   cannot run there.
2. **Spawn FreeRTOS tasks only post-boot** — from an `lv_async_call` or a screen
   callback (which run on the main thread after the scheduler thread is up),
   **never from `app_main()` before `vTaskStartScheduler()`**. Creating a task
   before the scheduler makes the port's one-time signal setup (`pthread_once`)
   run on the wrong thread and corrupts it.
3. **macOS/arm64 port deadlock — patched.** The port creates each task pthread
   inside a critical section that masks every signal; on macOS that deadlocks in
   libsystem once another task thread is parked in `pthread_cond_wait`, so
   `vTaskStartScheduler()` hangs creating the timer task (symptom: `xTaskCreate`
   returns pdPASS but the task body never runs). Fixed by
   `simulator/patches/freertos_posix_macos.py`, applied automatically via the
   `PATCH_COMMAND` on the `freertos_kernel` `FetchContent_Declare` in
   `simulator/CMakeLists.txt`. NameCardKnot has the same latent bug (its sim
   FreeRTOS never actually ran). If you bump the FreeRTOS-Kernel `GIT_TAG`,
   re-check this patch still applies (it self-skips and warns if the anchor moved).

Other simulator notes:
- LVGL config is `simulator/lv_conf.h` (found via `LV_CONF_INCLUDE_SIMPLE` +
  the project source dir). `LV_COLOR_DEPTH 16` (RGB565, matches the panel) and
  `LV_USE_THEME_DEFAULT 1` (the e-paper-derived base config had it off).
- ESP-IDF API shims for shared code: `simulator/tab5-bsp_simulator/inc/esp_err.h`,
  `esp_log.h`; FreeRTOS include-path shims in `simulator/freertos_shim/`.
- Build artifacts (`build/`, `simulator/.deps/`) are gitignored.
