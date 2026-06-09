# idf_compat — host compatibility layer for the simulator

Makes the ESP-IDF / device-only APIs that shared code (`app/`, `components/`)
relies on available when building `app/` on the desktop simulator. This is a
**simulator-only** component — on device the real ESP-IDF provides all of this,
so `idf_compat` is never part of the device build.

It is consumed by `simulator/CMakeLists.txt` via its `idf_component_register`
shim, which folds the `SRCS` / `INCLUDE_DIRS` declared in `CMakeLists.txt`
straight into the `simulator` executable (one binary → no separate library).

## What's here

```
idf_compat/
  include/            shim headers (what shared code #includes)
    esp_err.h esp_log.h esp_check.h esp_timer.h esp_heap_caps.h nvs.h nvs_flash.h
    driver/           jpeg_decode.h ppa.h
    hal/              ppa_types.h color_types.h dma2d_types.h  (PPA type headers)
    freertos/         host FreeRTOS API: FreeRTOS.h task.h queue.h semphr.h
                      event_groups.h timers.h portmacro.h
  src/                shim implementations
    esp_err.c esp_timer.c esp_heap_caps.c nvs.c jpeg_decode.c ppa.c
    freertos_port.c freertos_task.c freertos_queue.c
    freertos_event_groups.c freertos_timers.c
    freertos_internal.h   (shared helpers; not part of the public API)
  CMakeLists.txt      registers all of the above
  README.md           this file — the single source of truth for idf_compat
```

Everything here is a **hand-written host implementation** of a device API —
nothing is vendored. The same philosophy throughout: reimplement the API
*contract* on host primitives, just enough for the simulator.

- ESP-IDF APIs: `esp_err`, `esp_log`, `esp_check`, `esp_timer`, `esp_heap_caps`,
  and a JSON-backed `nvs` / `nvs_flash`.
- The FreeRTOS API (`freertos/*.h`) on native pthreads — see below.
- `driver/jpeg_decode` — IDF JPEG decode engine API, backed by libjpeg.
- `driver/ppa` — IDF PPA (Pixel-Processing Accelerator) API, a CPU impl — see below.

## Layout rule (how to add a new shim)

Mirror the **`#include` string**, not ESP-IDF's component boundaries (in a single
host binary, component splits like esp_common / nvs_flash are invisible):

- Included as a bare name (`"esp_err.h"`) → header in `include/`.
- Included with a path prefix (`"freertos/FreeRTOS.h"`, `"driver/gpio.h"`) → put
  the header under `include/<prefix>/`. `include` is the only `INCLUDE_DIRS`
  entry, so `"freertos/FreeRTOS.h"` resolves via that path; headers reference
  their siblings the same way (`#include "freertos/portmacro.h"`).
- Any `.c`/`.cpp` → `src/`, and add it to `SRCS` in `CMakeLists.txt`. That's the
  only wiring step.

## NVS

`nvs.c` implements the ESP-IDF NVS C API backed by a JSON file (default
`nvs_data.json` in the cwd; override with the sim-only `nvs_flash_sim_set_path()`
before the first open). Shared code calls the C API directly — there is no C++
wrapper. Fidelity notes are at the top of `src/nvs.c`.

## PPA (Pixel-Processing Accelerator)

`driver/ppa.h` + `hal/ppa_types.h` + `hal/color_types.h` (+ a minimal
`hal/dma2d_types.h`) mirror the ESP-IDF PPA API; `src/ppa.c` runs the operations
in plain C on the CPU so app code that offloads scale/rotate/mirror, blend and
fill to the P4 PPA HW can be developed on the desktop and previewed in the
simulator. The header struct/enum surface is kept in sync with the device (the
color-mode ids are the same `COLOR_TYPE_ID`-derived values), so app code is
source-portable across both targets.

Supported: ARGB8888 / RGB888 / RGB565 (the RGB display formats) for SRM in/out,
blend bg/fg/out and fill out, plus A8 / A4 blend foreground. **Not** implemented
(return `ESP_ERR_NOT_SUPPORTED` / log): YUV420 / YUV444 color modes and blend
color-keying. SRM scaling is bilinear (anti-aliased, like the HW) and rotation
is counter-clockwise (`PPA_SRM_ROTATION_ANGLE_*`). All ops run synchronously
regardless of `PPA_TRANS_MODE_*`, firing `on_trans_done` inline; burst length /
pending-transaction count / buffer alignment are accepted and ignored. Full
fidelity notes are at the top of `src/ppa.c`.

## FreeRTOS API (host, on pthreads)

`freertos/*.h` + `src/freertos_*.c` reimplement the FreeRTOS API on native
pthreads. **This is not the FreeRTOS kernel.** There is no scheduler, no tick
ISR, no signal machinery: a task *is* a detached pthread run by the host OS.

This replaced a previously vendored FreeRTOS-Kernel GCC/Posix port. That port ran
the real kernel and emulated a single-core, signal-driven scheduler on pthreads,
which forced fragile constraints (SDL had to own the main thread while the
scheduler ran on a background thread because `vTaskStartScheduler()` blocks
forever; tasks could only be created post-boot or the port's `pthread_once`
signal setup corrupted; two macOS/arm64 port bugs). Since the host OS already
schedules threads, that emulation bought fidelity this simulator doesn't need.
With the compat layer, `xTaskCreate()` works anywhere, anytime.

### Surface

- **Tasks** (`task.h` / `freertos_task.c`): `xTaskCreate`,
  `xTaskCreatePinnedToCore` (core ignored), `vTaskDelete` (self-delete robust;
  other-task is best-effort `pthread_cancel`), `vTaskDelay`, `xTaskDelayUntil`,
  `xTaskGetTickCount`, `xTaskGetCurrentTaskHandle`, priority get/set (stored, not
  enforced), and direct-to-task notifications (`xTaskNotifyGive`/`ulTaskNotifyTake`
  /`xTaskNotify`/`xTaskNotifyWait`).
- **Queues** (`queue.h` / `freertos_queue.c`): ring buffer + mutex + two condvars.
- **Semaphores** (`semphr.h`, built on the queue object): binary, counting,
  mutex, recursive mutex.
- **Event groups** (`event_groups.h` / `freertos_event_groups.c`).
- **Software timers** (`timers.h` / `freertos_timers.c`): one daemon pthread.
- **Port** (`portmacro.h` / `freertos_port.c`): heap (`pvPortMalloc`), ticks,
  critical sections, `vTaskStartScheduler` (blocks forever — unnecessary on host).

### Semantics that differ from real FreeRTOS

- **Critical sections → one global recursive mutex** (the "big kernel lock").
  Hardware runs one task at a time per core, so a critical section is atomic
  against all tasks; pthreads run in parallel, so `taskENTER_CRITICAL` /
  `portENTER_CRITICAL` (incl. the ESP-IDF `portMUX_TYPE*` form — arg accepted and
  ignored) take a global lock to restore single-at-a-time *inside* critical
  sections. Ordinary task code is genuinely parallel.
- **Priorities are not enforced** — host OS scheduling, best-effort.
- **No ISRs** — `*FromISR` variants forward to the blocking calls (zero timeout)
  and `portYIELD_FROM_ISR` is a no-op.
- **Tick rate** `configTICK_RATE_HZ = 100`, matching the device, so
  `pdMS_TO_TICKS()` and raw tick delays match both targets. Tick is derived from
  `CLOCK_MONOTONIC`, not a periodic interrupt.

### How to grow it

Add functions to the relevant `freertos_*.c` and declare them in the matching
`freertos/*.h` (new file → add to `SRCS`). Time-bounded waits use the
`freertos_cond_timedwait_ms()` / `freertos_now_ms()` helpers in
`freertos_internal.h`. Keep it demand-driven — implement what app code actually
uses, like the `esp_*` shims.
