# Tab5-ADB
ADB Client on M5Stack Tab5

## Development

Everything runs in the dev shell (`nix develop`, or direnv with `.envrc`).
`devkit` is described in `esp-devkit/docs/devkit-cli.md`.

```sh
devkit run                            # build, flash the Tab5 and follow its log
devkit run simulator                  # build and start the simulator
devkit use esp32p4 <device>           # pin the board when several are plugged in
```

The phone goes into the Tab5's USB-A port. The simulator takes a phone plugged
into this computer with `devkit cmd "usb attach usb <vid:pid>"` (`usb list`
shows them), after `adb kill-server` releases it.

While `devkit run` or `devkit monitor` holds the target, other terminals drive
it:

```sh
devkit monitor --backlog 50           # the log so far, then follow it
devkit cmd "tap 360 512" "settle" "capture home.jpg"
devkit cmd "adb tcp <host:port>"      # connect over TCP from the home screen
```

Unit tests run on the host, per crate, once the simulator has been built:

```sh
cmake --build simulator/build --target rust_test_adb    # also rust_test_agent_link, rust_test_app
```

After changing `android-agent/src`, rebuild the jar the firmware embeds,
`app/agent/tab5adb-agent.jar`, and commit it with the change:

```sh
android-agent/build.sh
```

The protocol between the Tab5 and the agent is `android-agent/docs/protocol.md`.

## Testing with the Android emulator

The flake runs Android emulators for the versions without a device at hand:
`emulator-28` (Android 9), `emulator-31` (12), `emulator-36` (16) and
`emulator-37` (17). The first run downloads the system image.

```sh
nix run .#emulator-36                 # add -- -no-window to run it headless
devkit run simulator
```

On the simulator's home screen, connect to `10.0.2.2:5555` under Wireless;
`10.0.2.2` is this computer's `127.0.0.1` as the simulator sees it. A script
does the same with:

```sh
devkit cmd "adb tcp 10.0.2.2:5555"
```

- The emulator's ADB port is fixed at 5555, so one emulator runs at a time.
- The virtual devices live in `.emulator/` under the working directory, or in
  `$TAB5_ADB_EMULATOR_HOME`; run it from the repository root.
