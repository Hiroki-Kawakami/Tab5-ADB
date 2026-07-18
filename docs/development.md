# Development

## Build environment

The ESP-IDF (v6.0.2) toolchain and the host-simulator tools (cmake, ninja, gcc,
SDL2, cjson, libusb, Mbed TLS 4) all come from a Nix flake. The base development
shell is inherited from the pinned `esp-devkit` submodule. **Always run commands
through `nix develop -c <cmd>`** (or from inside a `nix develop` shell) — never
invoke `idf.py` / `cmake` / `esptool` directly.

Initialize the submodule after cloning an existing checkout with
`git submodule update --init --recursive`. Nix flakes only see git-tracked
files. After adding new files, `git add` them
(staging is enough) or `nix develop` won't pick up `flake.nix` changes / the
tree, and the build can fail with "not tracked by Git".

## Device — `esp32p4/` (ESP32-P4 / M5Stack Tab5)

```sh
nix develop -c idf.py -C esp32p4 build
nix develop -c idf.py -C esp32p4 flash monitor   # needs a TTY
nix develop -c ./run.sh esp32p4                   # same as flash monitor
```

When `idf.py monitor` can't attach (no TTY in a non-interactive shell), drive
the serial directly with esptool + PySerial. The second `/dev/cu.usbmodem*`
enumerator is the JTAG/console port used for flashing; **ESP32-P4 only prints
logs once after reset**, so capture during the boot sequence.

`sdkconfig` is gitignored and only regenerated from `sdkconfig.defaults` when
absent. After editing `esp32p4/sdkconfig.defaults`, run
`rm esp32p4/sdkconfig && idf.py -C esp32p4 reconfigure` and grep the generated
`sdkconfig` to confirm the option took.

## Host simulator — `simulator/`

Runs `app/` on the desktop (SDL2 + LVGL + a pthread-backed FreeRTOS API — see
[architecture.md](architecture.md#freertos-on-the-host)) so UI / app logic can be
developed without a board.

```sh
nix develop -c ./run.sh
```

`run.sh simulator` only configures when `build/` is missing; use
`nix develop -c cmake --fresh -S simulator -B build -G Ninja` to force a clean
reconfigure after changing the simulator component graph.

## Headless UI verification (simverify)

Drives the UI from a script with no host window — synthetic touch in, captured
JPEG frames out — instead of clicking/screenshotting the real SDL window
(fragile, host-state-dependent, hands the agent host-PC control). The same
`simulator` binary runs the same app/BSP code with `SIMULATOR_HEADLESS=1` +
`SIMULATOR_SCRIPT=<path>`; see `esp-devkit/sim_harness/` for the
interpreter and `simulator/verify/` for examples.

```sh
nix develop -c ./run.sh simverify simulator/verify/home.txt
nix develop -c ./run.sh simverify simulator/verify/mirror.txt
nix develop -c ./run.sh simverify simulator/verify/wifi.txt
```

Script commands: `wait <ms>`, `settle [<max_ms>]`, `capture <path.jpg>`,
`tap/down/move/up <x> <y>`, `quit`, plus the Wi-Fi fake-backend controls
`wifi-aps`/`wifi-connect-result`/`wifi-delay`/`wifi-drop`. `capture` auto-creates
its output directory; scripts write to the gitignored `simulator/verify/out/`.
Some scripts need a real Android phone plugged into the PC (mirror, media, file
transfer, install) — read the script's header comment before running it, and
clean up anything it leaves on the phone/SD card (installed test APKs, pushed
test files) afterward.

Two gotchas baked into the harness, worth knowing before writing a new script:
- `tap` spans a press *and* release edge so both a BSP dispatch-task sample
  and an LVGL indev read see it; `down`/`move`/`up` scripts need `wait`s between
  steps for a drag to be sampled (each step only writes the touch snapshot).
- `settle` pumps `lv_timer_handler` until no animation runs for a few frames —
  capturing before the frame settles is a scripting bug, not a harness one.

## Android agent — `android-agent/`

```sh
nix develop -c android-agent/build.sh      # javac + d8 -> build/tab5adb-agent.jar
nix develop -c android-agent/run.sh        # adb push + app_process (foreground)
# from another shell, reach the socket from the PC:
nix develop -c adb forward tcp:8080 localabstract:tab5adb-agent
```

The agent is developed and debugged against a **real phone plugged into the PC
with standard adb** (push + `app_process` + `adb forward localabstract:`), so
Tab5 USB wiring isn't on the critical path when iterating on the Java side. See
`android-agent/README.md`.

**After editing Java under `android-agent/src/`, rebuild the jar AND regenerate
the embedded C array** (`app/agent/agent_jar.{h,c}`, an `xxd -i` of
`android-agent/build/tab5adb-agent.jar`) — the Tab5 firmware and simverify run
the *embedded* jar, not the on-disk one, so a rebuild without regenerating the
header silently keeps testing the old agent.

## Host test runners

Every component with host-testable logic has its own `test/run.sh`, invoked via
`nix develop -c <path>/run.sh` with `TEST=<name>` selecting `test/<name>.cpp`
(default is the first/no-phone test). Artifacts go to each `test/build/`
(gitignored).

| Component | Runner | Notes |
|---|---|---|
| `app/` (parsers) | `app/test/run.sh` | `test_device_info` / `test_apk_info` / `test_media_session` / `test_sysclock` — no phone, no LVGL |
| `esp-devkit/bsp/` | `esp-devkit/bsp/test/run.sh` | `test_audio_dsp` (default, pure math) / `test_bsp_audio` (dispatch policy vs stub) / `test_sdl_audio` (audible pacing check) — no device |
| `components/term_emu/` | `components/term_emu/test/run.sh` | VT parser + grid, incl. a chunk-split fuzz — no phone |
| `components/embedded_adb/` | `components/embedded_adb/test/run.sh` | `test_crypto` (default, no phone) / `test_connect` / `test_shell` (need an authorized phone over libusb) / `test_connect_tcp` (`TAB5ADB_TCP_TARGET=host:port` env, no IP committed to git) |
| `components/adb/` | `components/adb/test/run.sh` | `test_client` (default) / `test_shell` / `test_sync` — all need a phone connected + authorized over libusb |
| `components/agent_link/` | `components/agent_link/test/run.sh` | `test_hello` (default) / `test_mirror` — libusb vs a real phone; decodes strips with host libjpeg |
| `android-agent/` | `android-agent/test/run.sh` | host-JVM `ProjectionTest` — no phone |

All the libusb-based runners run `adb kill-server` first so the host adb-server
doesn't hold the USB interface.
