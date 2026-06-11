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

`simulator/` runs `app/` on the desktop (SDL2 + LVGL), with a **pthread-backed
FreeRTOS API** so app code can use FreeRTOS primitives (`xTaskCreate`,
`vTaskDelay`, queues, semaphores, event groups, timers) off-device.

```sh
./run.sh            # == ./run.sh simulator
# equivalent to:
# nix develop -c sh -c 'cmake --fresh -S simulator -B build -G Ninja && cmake --build build && ./build/simulator'
```

`app/` talks to hardware through the shared BSP (`bsp_*`); the simulator
implements that seam with an SDL backend inside `components/m5stack-bsp/`
(`simulator/sdl_backend.cpp` + the tab5 sim board `boards/tab5/tab5_sim.cpp`), so
`app/` compiles unchanged. `simulator/platform/main.cpp` only owns the host LVGL
runtime (the SDL/LVGL timer loop).

Notes:
- SDL/LVGL own the main thread (SDL must be driven from the main thread on
  macOS), so `main()` runs the `lv_timer_handler` loop there. That is the only
  main-thread rule — it mirrors device `app_main()`.
- FreeRTOS tasks are just pthreads (no scheduler to start), so `xTaskCreate()`
  works anywhere — including directly from `adb_app()`, like on device.
- The FreeRTOS API is reimplemented on pthreads in `simulator/idf_compat/`
  (`include/freertos/*.h` + `src/freertos_*.c`), not the real kernel. One
  semantic gap: critical sections use a global lock (host tasks run in parallel).
  See `simulator/idf_compat/README.md` for the surface and details.

### Headless UI verification

Drive the UI from a script with no host window — synthetic touch in, captured
JPEG frames out — instead of clicking/screenshotting the real window:

```sh
./run.sh simverify simulator/verify/home.txt     # build + run headless, capture frames
./run.sh simverify simulator/verify/mirror.txt   # connect a phone + show the live mirror
```

The script (`tap`/`down`/`move`/`up`, `wait`/`settle`, `capture <path.jpg>`,
`quit`) runs the same `app/` code under `SIMULATOR_HEADLESS`; see
`simulator/platform/sim_harness.h` and the examples in `simulator/verify/`.

## Layout

- `app/` — application screens and logic (shared by device + simulator).
- `components/` — shared components (both targets): `m5stack-bsp/` (board support
  behind the `bsp.h` API — device chip drivers in `devices/`, the SDL simulator
  backend in `simulator/`, and per-model bring-up in `boards/<model>/` with a
  device `.c` and a simulator `_sim.cpp`), `lvgl++/` (C++ helpers over LVGL),
  `screen_manager/` (screen stack / navigation), `embedded_adb/` (ADB host-side
  client library in C++ — portable protocol/crypto/auth with a USB transport that
  splits esp-idf `usb_host` on device vs `libusb` in the simulator), and `adb/`
  (the app-facing object API over `embedded_adb` — owns the connection lifecycle
  and exposes `Client`/`Shell`/`Sync`; see `components/adb/README.md`). For screen
  mirroring: `agent_link/` (the Tab5 end of the `tab5adb-agent` wire protocol) and
  `jpeg_decode_enhanced/` (enhanced P4 HW JPEG decode — full-range YUV→RGB,
  strip pipelining, PPA pipeline; libjpeg-backed in the simulator). For the
  terminal: `term_emu/` (a VT100/xterm-subset terminal emulator — PTY bytes in,
  cell grid out; no LVGL/adb deps, host unit test via its `test/run.sh`) behind
  the **Shell** screen's grid renderer + on-screen keyboard (`app/terminal/`).
- `esp32p4/` — device build root (IDF project): `main/` is the entry point
  (`app_main` + device LVGL runtime via esp_lvgl_port).
- `simulator/` — host SDL2/LVGL build of `app/`: `platform/` is the SDL/LVGL
  entry; `idf_compat/` is the host compat component (ESP-IDF API shims + a
  pthread-backed FreeRTOS API — see its `README.md`).
- `android-agent/` — `tab5adb-agent`, the Android-side companion (screen
  mirroring + offload). A scrcpy-style Java program launched via `app_process`
  (no APK); built with the Android SDK from the flake. See its `README.md`. The
  app's **Mirroring** screen (`ADBMirroringScreen`) pushes the embedded jar
  (`app/agent/agent_jar.*`), launches it, and renders its live JPEG stream.
