# Tab5-ADB

ADB (Android Debug Bridge) client on M5Stack Tab5 (ESP32-P4).

## Build / flash / monitor

The dev environment lives in a Nix flake; use `nix develop`:

```sh
nix develop --command idf.py -C esp32p4 build
nix develop --command idf.py -C esp32p4 flash monitor   # needs a TTY
# or:
./run.sh
```

## Layout

- `esp32p4/` — IDF project root (CMakeLists, sdkconfig.defaults, partitions).
- `idf-components/main/` — entry point + platform port (`pf_port`).
- `idf-components/m5tab5-bsp/` — board support: display, touch, audio, power.
- `components/lvgl++/` — small C++ helpers over LVGL.
- `components/screen_manager/` — screen stack / navigation.
- `app/` — application screens and logic.
