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
abstraction (`simulator/tab5-bsp_simulator/`) so `app/` compiles unchanged; the
device implementation lives in `idf-components/main/main.cpp`.

Notes:
- SDL/LVGL own the main thread; `vTaskStartScheduler()` runs on a background
  pthread (`simulator/src/main.cpp`), required because SDL must be driven from
  the main thread on macOS.
- Spawn FreeRTOS tasks from post-boot code (e.g. an `lv_async_call` or a screen
  callback), never from `app_main()` before the scheduler starts.
- `simulator/patches/freertos_posix_macos.py` patches the fetched FreeRTOS
  kernel to fix a macOS/arm64 deadlock when creating task pthreads (applied
  automatically via the CMake `PATCH_COMMAND`).

## Layout

- `esp32p4/` — IDF project root (CMakeLists, sdkconfig.defaults, partitions).
- `idf-components/main/` — entry point + platform port (`pf_port`).
- `idf-components/m5tab5-bsp/` — board support: display, touch, audio, power.
- `components/lvgl++/` — small C++ helpers over LVGL.
- `components/screen_manager/` — screen stack / navigation.
- `app/` — application screens and logic (shared by device + simulator).
- `simulator/` — host SDL2/LVGL + FreeRTOS-POSIX build of `app/`.
