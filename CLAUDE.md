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

### Component layout — one directory per category

```
app/                         # SHARED  app logic / screens (a single component)
components/                  # SHARED  (both targets)
  platform_port/             #   pf_port interface (platform_port.hpp), header-only
  lvgl++/                    #   C++ helpers (lv_async_call, lv_obj_add_event_fn)
  screen_manager/            #   Screen base + screen stack
esp32p4/                     # DEVICE build root (IDF project)
  main/                      #   entry point + on-device pf_port impl + nvs.hpp
  components/m5tab5-bsp/     #   DEVICE-only board support (auto-discovered by IDF)
simulator/                   # SIMULATOR build root (see below)
  platform/                  #   SIM-only entry + pf_port impl (SDL) + nvs.hpp
  idf_compat/                #   SIM-only ESP-IDF API shims (esp_err/esp_log/freertos)
  freertos/                  #   SIM-only FreeRTOS POSIX config + macOS patch
```

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
  **shim** (a CMake function) and `add_subdirectory`s each shared component; the
  shim folds the component's `SRCS`/`INCLUDE_DIRS` straight into the `simulator`
  executable. `REQUIRES` are ignored (one binary → includes are global; IDF-only
  requirements like `esp_lvgl_port` have no host counterpart).

### The `pf_port` seam

`app/` is platform-agnostic; the seam is the `pf_port` namespace
(`components/platform_port/platform_port.hpp` — interface only):

- Device implementation: `esp32p4/main/main.cpp` (BSP + esp_lvgl_port).
- Simulator implementation: `simulator/platform/platform_port_sim.cpp`
  (SDL window/texture for display, mouse for touch). Same header, so `app/`
  compiles unchanged.

`app_main()` lives in each platform's port file (device `main.cpp`; simulator
`platform_port_sim.cpp`) and just calls `adb_app()` in `app/adb_app.cpp`, which
sets up the LVGL display on the two `pf_port` frame buffers and pushes the first
screen. Panel is 720×1280 portrait RGB565 (`PANEL_W`/`PANEL_H` in `app/adb_app.hpp`).

`nvs.hpp` is a **per-platform seam like `pf_port`** (not shared): two different
header-only implementations of the same `NVS` API live with each port
(`esp32p4/main/nvs.hpp` = nvs_flash; `simulator/platform/nvs.hpp` = JSON file).
It is not on `app/`'s include path today; if shared code needs NVS, promote it to
a shared interface component first.

## Simulator details (FreeRTOS POSIX port)

The simulator links the **FreeRTOS-Kernel GCC/Posix port** so app code can use
real FreeRTOS primitives (`xTaskCreate`, `vTaskDelay`, queues, semaphores) on the
host. Verified working end-to-end (task entry → `vTaskDelay` ticks → `vTaskDelete`).

Hard constraints — violate these and it hangs/crashes:

1. **SDL/LVGL own the main thread.** `simulator/platform/main.cpp` runs the LVGL/SDL
   loop on the main thread and starts `vTaskStartScheduler()` on a *background
   pthread*. SDL must be driven from the main thread on macOS, so the scheduler
   cannot run there.
2. **Spawn FreeRTOS tasks only post-boot** — from an `lv_async_call` or a screen
   callback (which run on the main thread after the scheduler thread is up),
   **never from `app_main()` before `vTaskStartScheduler()`**. Creating a task
   before the scheduler makes the port's one-time signal setup (`pthread_once`)
   run on the wrong thread and corrupts it.
3. **Two macOS/arm64 POSIX-port bugs — both patched** by
   `simulator/freertos/patches/freertos_posix_macos.py`, applied via the
   `PATCH_COMMAND` on the `freertos_kernel` `FetchContent_Declare` in
   `simulator/CMakeLists.txt`. The script applies independent hunks, each guarded
   by its own marker, so it self-skips per-hunk and warns if an anchor moved.
   **If you bump the FreeRTOS-Kernel `GIT_TAG`, re-check both still apply.**
   - **Task-creation deadlock** (marker `macOS/arm64 workaround`): the port
     creates each task pthread inside a critical section that masks every signal;
     on macOS that deadlocks in libsystem once another task thread is parked in
     `pthread_cond_wait`, so `vTaskStartScheduler()` hangs creating the timer
     task (symptom: `xTaskCreate` returns pdPASS but the task body never runs).
     Fix: create the task thread with a clean signal mask, then restore.
     NameCardKnot has the same latent bug (its sim FreeRTOS never actually ran).
   - **Sub-page stack-size underflow** (marker `macOS/arm64 stack-size fix`):
     `pxPortInitialiseStack()` rounds the stack *end pointer* up to a page, but
     arm64 pages are 16 KB while a task stack (`configMINIMAL_STACK_SIZE` words ×
     8 B ≈ 2 KB for idle) is smaller than one page. The rounding pushes the end
     past the top of stack, the size subtraction underflows to a huge `size_t`,
     and `pthread_create()` faults (`EXC_BAD_ACCESS`, surfaces as `Bus error: 10`)
     while `vTaskStartScheduler()` creates the idle task. **Non-deterministic** —
     depends on each stack buffer's malloc alignment, so it can appear to "work"
     on lucky runs. Fix: round the *size* up to a page instead (never underflows);
     `PTHREAD_STACK_MIN` is the floor.

Other simulator notes:
- LVGL config is `simulator/lv_conf.h` (found via `LV_CONF_INCLUDE_SIMPLE` +
  the project source dir). `LV_COLOR_DEPTH 16` (RGB565, matches the panel) and
  `LV_USE_THEME_DEFAULT 1` (the e-paper-derived base config had it off).
- ESP-IDF API shims for shared code all live in `simulator/idf_compat/`:
  `esp_err.h`/`esp_err.c`, `esp_log.h`, and the `freertos/` include-path shims
  (`freertos/FreeRTOS.h` → real `<FreeRTOS.h>`). `idf_compat/` is on the include
  path; `idf_compat/freertos/` is **not** (so the shim's `#include <task.h>`
  reaches the kernel, not itself).
- Build artifacts (`build/`, `simulator/.deps/`) are gitignored.
