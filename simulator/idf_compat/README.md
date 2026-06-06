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
    freertos/         bridge headers: "freertos/FreeRTOS.h" -> <FreeRTOS.h>
  src/                shim implementations
    esp_err.c esp_timer.c esp_heap_caps.c nvs.c
  freertos_kernel/    VENDORED upstream FreeRTOS-Kernel (see below)
  CMakeLists.txt      registers all of the above
  README.md           this file — the single source of truth for idf_compat
```

Two kinds of thing live here, on purpose:

- **Hand-written shims** (`include/` + `src/`) — our own host implementations of
  ESP-IDF APIs: `esp_err`, `esp_log`, `esp_check`, `esp_timer`, `esp_heap_caps`,
  and a JSON-backed `nvs` / `nvs_flash`. These reimplement just enough of each
  API for the host.
- **Vendored upstream** (`freertos_kernel/`) — a real third-party library we host
  a frozen copy of, rather than reimplementing. Keep the ownership boundary
  clear: don't hand-edit files under `freertos_kernel/` except for the documented
  macOS fixes below.

## Layout rule (how to add a new shim)

Mirror the **`#include` string**, not ESP-IDF's component boundaries (in a single
host binary, component splits like esp_common / nvs_flash are invisible):

- Included as a bare name (`"esp_err.h"`) → header in `include/`.
- Included with a path prefix (`"freertos/FreeRTOS.h"`, `"driver/gpio.h"`) → put
  the header under `include/<prefix>/`. That prefix subdir is reachable as part
  of the include string but is **not** itself an `INCLUDE_DIRS` entry, so a
  bridge header's `#include <foo.h>` resolves to the real impl, not to itself.
- Any `.c`/`.cpp` → `src/`, and add it to `SRCS` in `CMakeLists.txt`. That's the
  only wiring step.

## NVS

`nvs.c` implements the ESP-IDF NVS C API backed by a JSON file (default
`nvs_data.json` in the cwd; override with the sim-only `nvs_flash_sim_set_path()`
before the first open). Shared code calls the C API directly — there is no C++
wrapper. Fidelity notes are at the top of `src/nvs.c`.

## Vendored FreeRTOS-Kernel

- **Upstream:** https://github.com/FreeRTOS/FreeRTOS-Kernel
- **Version:** V11.1.0
- **Port:** `portable/ThirdParty/GCC/Posix` (GCC/Posix), heap: `MemMang/heap_3.c`

Only the subset needed for the Posix port is vendored: the core sources
(`tasks.c`, `queue.c`, `list.c`, `timers.c`, `event_groups.c`,
`stream_buffer.c`), all of `include/`, the Posix port (`port.c`, `portmacro.h`,
`utils/wait_for_event.{c,h}`), `heap_3.c`, `FreeRTOSConfig.h`, and `LICENSE.md`.
`croutine.c` is omitted (`configUSE_CO_ROUTINES` is 0).

### Local modifications

Two macOS/arm64 fixes are applied **inline** to
`freertos_kernel/portable/ThirdParty/GCC/Posix/port.c`, each marked with a
comment so they're easy to find and re-apply. Grep `macOS/arm64`:

1. **`macOS/arm64 workaround`** — in `pxPortInitialiseStack()`, create the task
   pthread with a clean signal mask (the port otherwise calls `pthread_create()`
   with every signal masked, which deadlocks libsystem on macOS).
2. **`macOS/arm64 stack-size fix`** — in `pxPortInitialiseStack()`, round the
   stack *size* up to a page instead of rounding the *end pointer* up (the latter
   underflows to a huge `size_t` for sub-page task stacks on 16 KB-page arm64 and
   crashes `pthread_create`).

There are no other edits — every other vendored file is verbatim upstream.

### How to upgrade

1. Clone the new tag of FreeRTOS-Kernel somewhere outside the repo.
2. Overwrite the vendored files from the same upstream paths (the layout under
   `freertos_kernel/` mirrors upstream exactly, so it's a straight copy of: the
   six core `*.c`, `include/*.h`, the Posix port dir, `MemMang/heap_3.c`,
   `LICENSE.md`). Do **not** overwrite `FreeRTOSConfig.h` — it's ours.
3. Re-apply the two macOS fixes to `port.c` (search the new `port.c` for
   `pxPortInitialiseStack`; the anchors are the `pthread_create()` call and the
   `#ifdef __APPLE__` stack-size block). The previous diff is recoverable from
   git history of this file.
4. If the port added/renamed source files, update `SRCS` in `CMakeLists.txt`.
5. Build the simulator and run it: reaching the home screen means the scheduler,
   timer task, and idle task all came up — i.e. the fixes are in place. Run a few
   times (the stack-size bug was non-deterministic).
