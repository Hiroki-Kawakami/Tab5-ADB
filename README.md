# Tab5-ADB

ADB (Android Debug Bridge) client on M5Stack Tab5 (ESP32-P4).

## Build / flash / monitor

The dev environment lives in a Nix flake; use `nix develop`:

```sh
nix develop --command idf.py -C esp32p4 build
nix develop --command idf.py -C esp32p4 flash monitor   # needs a TTY
# or:
./run.sh esp32p4
```

## Host simulator

`simulator/` runs `app/` on the desktop (SDL2 + LVGL), with the **FreeRTOS POSIX
port** linked in so app code can use real FreeRTOS primitives (`xTaskCreate`,
`vTaskDelay`, queues, semaphores) off-device.

```sh
./run.sh            # == ./run.sh simulator
# equivalent to:
# nix develop -c sh -c 'cmake --fresh -S simulator -B build -G Ninja && cmake --build build && ./build/simulator'
```

The simulator provides a host (SDL) implementation of the `pf_port` platform
abstraction (`simulator/platform/platform_port_sim.cpp`) so `app/` compiles
unchanged; the device implementation lives in `esp32p4/main/main.cpp`.

Notes:
- SDL/LVGL own the main thread; `vTaskStartScheduler()` runs on a background
  pthread (`simulator/platform/main.cpp`), required because SDL must be driven
  from the main thread on macOS.
- Spawn FreeRTOS tasks from post-boot code (e.g. an `lv_async_call` or a screen
  callback), never from `app_main()` before the scheduler starts.
- `simulator/freertos/patches/freertos_posix_macos.py` patches the fetched
  FreeRTOS kernel for two macOS/arm64 POSIX-port bugs in task pthread creation
  (an all-signals-masked deadlock and a sub-page stack-size underflow), applied
  automatically via the CMake `PATCH_COMMAND`.

## Layout

- `app/` — application screens and logic (shared by device + simulator).
- `components/` — shared components (both targets): `platform_port/` (the
  `pf_port` interface), `lvgl++/` (C++ helpers over LVGL), `screen_manager/`
  (screen stack / navigation).
- `esp32p4/` — device build root (IDF project): `main/` is the entry point +
  on-device `pf_port` impl; `components/m5tab5-bsp/` is board support (display,
  touch, audio, power).
- `simulator/` — host SDL2/LVGL + FreeRTOS-POSIX build of `app/`: `platform/`
  is the SDL entry + host `pf_port` impl, `idf_compat/` holds host shims for
  ESP-IDF APIs, `freertos/` holds the POSIX config + macOS patch.
