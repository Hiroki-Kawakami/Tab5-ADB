# Tab5-ADB — Notes for Claude

ADB (Android Debug Bridge) client on M5Stack Tab5 (ESP32-P4). Scaffolded from
the `Tab5-UVC-Display` project (build system, BSP, LVGL infra) with a host
simulator modeled on `NameCardKnot`.

> **Keep this file current.** When you change the build flow, the simulator
> architecture, the `pf_port` surface, add a target, or hit a non-obvious gotcha
> worth remembering, update CLAUDE.md in the same change. It is the handoff to
> the next session.
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
  main/                      #   entry point + on-device pf_port impl
  components/m5stack-bsp/    #   DEVICE-only board support (auto-discovered by IDF)
    inc/                     #     model-agnostic public API (bsp.h) + bsp_audio.h, bsp_types.h
    inc_private/             #     internal driver interfaces (bsp_display.h / bsp_touch.h vtables)
    src/                     #     shared layer: bsp_display.c/bsp_touch.c (public API), audio_eq.c
    devices/                 #     reusable chip drivers (ili9881c/st7123/gt911/es8388/pi4io)
    boards/<model>/          #     per-model bring-up; tab5/ implements bsp_init()
simulator/                   # SIMULATOR build root (see below)
  platform/                  #   SIM-only entry + pf_port impl (SDL)
  idf_compat/                #   SIM-only ESP-IDF compat component (see its README.md)
    include/  src/           #     hand-written shims (esp_err/esp_log/esp_check/esp_timer/nvs)
    freertos_kernel/         #     vendored FreeRTOS-Kernel (Posix port + macOS fixes)
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
  **shim** (a CMake function) and `add_subdirectory`s each component; the shim
  folds the component's `SRCS`/`INCLUDE_DIRS` straight into the `simulator`
  executable. `REQUIRES` are ignored (one binary → includes are global; IDF-only
  requirements like `esp_lvgl_port` have no host counterpart). Both the shared
  components and the **simulator-only `idf_compat` component** (its own
  `CMakeLists.txt`) are consumed this way — so growing `idf_compat` just means
  adding `SRCS` to that file, not editing `simulator/CMakeLists.txt`. Only
  `simulator/platform/` (the SDL/LVGL entry + pf_port impl) stays a direct
  `target_sources` — it's the executable's "main", not a component.

### Where a device/simulator-divergent API goes — the rule

When an API needs different device vs simulator implementations, decide by one
question: **does Espressif already define this API?**

- **Yes** (e.g. `esp_err`, `esp_log`, `nvs_flash`, FreeRTOS, `esp_timer`,
  `esp_heap_caps`) → implement the *ESP-IDF API itself* on the host in
  `simulator/idf_compat/`. Don't wrap it in `pf_port` — that would re-abstract
  something already abstracted, and app code stays standard ESP-IDF. One
  canonical home: never let an ESP-IDF compat shim grow inside the BSP or another
  component. (PSRAM allocation is `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` —
  an ESP-IDF API, so app code calls it directly on both targets.)
- **No** — it's this board's own hardware concern with no standard contract
  (framebuffer, touch point, brightness) → put it behind `pf_port`. Keep
  `pf_port` small and demand-driven; don't pre-build a speculative surface.

Ambiguous-looking cases resolve cleanly under this rule: `nvs_flash` is an
ESP-IDF API → it's `idf_compat` and app code calls the C API directly (see the
NVS section), **not** a per-platform `pf_port`-style seam.

### The `m5stack-bsp` component (device-side board support)

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
- **Drivers (`devices/`)** — reusable chip drivers, each a `bsp_display`/
  `bsp_touch` provider. They include only `bsp_display.h`/`bsp_touch.h`
  (+`bsp_types.h`), not `bsp_private.h`.

Audio (`bsp_tab5_audio_*` in `inc/bsp_audio.h`) is Tab5-specific with no
cross-model contract yet, so it keeps its name and lives in the tab5 board.

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

## Simulator details (FreeRTOS POSIX port)

The simulator compiles in the **FreeRTOS-Kernel GCC/Posix port**, vendored under
`simulator/idf_compat/freertos_kernel/` (see `simulator/idf_compat/README.md`
for the vendoring/upgrade details), so app code
can use real FreeRTOS primitives (`xTaskCreate`, `vTaskDelay`, queues,
semaphores) on the host. Verified working end-to-end (scheduler + idle + timer
task come up; task entry → `vTaskDelay` ticks → `vTaskDelete`).

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
3. **Two macOS/arm64 POSIX-port bugs — both fixed inline** in the vendored
   `freertos_kernel/portable/ThirdParty/GCC/Posix/port.c` (grep `macOS/arm64`).
   These are the only edits to the vendored kernel; re-apply them on upgrade (the
   procedure is in `simulator/idf_compat/README.md`). What they fix:
   - **Task-creation deadlock** (marker `macOS/arm64 workaround`): the port
     creates each task pthread inside a critical section that masks every signal;
     on macOS that deadlocks in libsystem once another task thread is parked in
     `pthread_cond_wait`, so `vTaskStartScheduler()` hangs creating the timer
     task (symptom: `xTaskCreate` returns pdPASS but the task body never runs).
     Fix: create the task thread with a clean signal mask, then restore.
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
- Host compat for shared code lives in the `simulator/idf_compat/` component.
  **`simulator/idf_compat/README.md` is the source of truth** (layout, the
  include-path rules, how to add a shim, FreeRTOS vendoring/upgrade). In short:
  `include/` = shim headers, `src/` = shim sources, `freertos_kernel/` = vendored
  upstream FreeRTOS. Current shim surface:
  - `esp_err` — `esp_err_t`, `ESP_ERR_*`, `esp_err_to_name()`, `ESP_ERROR_CHECK[_WITHOUT_ABORT]`.
  - `esp_log` — `ESP_LOGx` to stderr, level enum, `esp_log_level_set` (no-op).
  - `esp_check` — `ESP_RETURN_ON_ERROR` / `ESP_GOTO_ON_*` / `ESP_RETURN_ON_FALSE`.
  - `esp_timer` — `esp_timer_get_time()` (monotonic µs) only.
  - `esp_heap_caps` — `heap_caps_malloc`/`calloc`/`realloc`/`aligned_alloc`/`free`
    (host malloc; `MALLOC_CAP_*` flags accepted and ignored).
  - `nvs`/`nvs_flash` — JSON-backed NVS C API (see the NVS section).
  - `include/freertos/` — bridge headers (`freertos/FreeRTOS.h` → real `<FreeRTOS.h>`).
  `INCLUDE_DIRS` is `include` + the vendored kernel dirs; `include/freertos/` is
  deliberately **not** on the path (so the bridge's `#include <task.h>` reaches
  the kernel, not itself). Add a shim: header in `include/`, source in `src/` +
  its `SRCS` entry — nothing else to wire.
- Build artifacts (`build/`, `simulator/.deps/`) are gitignored. LVGL is fetched
  into `.deps`; FreeRTOS is vendored in-tree.
