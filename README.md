# Tab5-ADB

ADB (Android Debug Bridge) client on M5Stack Tab5 (ESP32-P4).

Clone with the required submodules:

```sh
git clone --recurse-submodules https://github.com/Hiroki-Kawakami/Tab5-ADB.git
cd Tab5-ADB
```

For an existing checkout, run `git submodule update --init --recursive` once.

## Development

The dev environment lives in a Nix flake; always run build tooling through
`nix develop`:

```sh
nix develop -c idf.py -C esp32p4 build
nix develop -c ./run.sh esp32p4
nix develop -c ./run.sh
nix develop -c ./run.sh simverify simulator/verify/home.txt
```

Reusable BSP, simulator compatibility, Wi-Fi, image and LVGL infrastructure
comes from the pinned [`esp-devkit`](esp-devkit/) submodule.
The ADB host stack comes from the pinned
[`esp-adb-host`](components/adb/) submodule. It is mounted at
`components/adb`, so both the ESP-IDF build and the simulator consume the same
`adb` component without target-specific dependency setup.

See [`docs/development.md`](docs/development.md) for every build/flash/test
command, and [`docs/architecture.md`](docs/architecture.md) for the component
layout and the rules that govern where new code goes. `CLAUDE.md` and
[`docs/`](docs/) are the full contributor/agent handoff (architecture, design
decisions, known gotchas).

## License

Tab5-ADB's own source code is licensed under the **MIT License** — see
[`LICENSE`](LICENSE). This is a personal, non-commercial project published as
source on GitHub plus prebuilt binaries; it comes with **no warranty and no
support** (as stated in the MIT disclaimer).

The firmware bundles third-party fonts, icons, libraries, and components under
their own licenses (Apache-2.0, SIL OFL 1.1, ISC, zlib, and MIT). Their full
texts and the project acknowledgements (including the adb / scrcpy reference
projects) are reproduced in [`app/third_party_licenses.hpp`](app/third_party_licenses.hpp),
which is also viewable on-device under **About → Acknowledgements**.
