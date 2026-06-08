# tab5adb-agent

Android-side companion for Tab5-ADB. Its main purpose is **screen mirroring**;
it also hosts processing the Tab5 can't do on its own (offload). Mirroring is one
service among several it exposes.

Like `scrcpy`'s server, it is **not an APK** — a plain Java program packaged as a
dex jar, pushed to `/data/local/tmp`, and launched with `app_process`. Run that
way it has the shell uid (2000), so it reaches the hidden Android APIs (display
capture, input injection) without a permission dialog. It listens on the abstract
socket `localabstract:tab5adb-agent`, which the Tab5 host reaches over its
embedded ADB.

## Status

The server listens on `localabstract:tab5adb-agent` and runs the protocol's
**HELLO handshake**: on each connection it sends its HELLO CONTROL_REQUEST and
checks the Tab5 CONTROL_RESPONSE (proto-version match), then holds the stream
open. Screen capture / offload services come later (Phase 2). Verified against a
real Android device by the headless Tab5-side harness `components/agent_link/test/test_hello.cpp`
(no GUI; drives connect → push → launch → HELLO over libusb).

The wire protocol — single-socket framing, the agent-initiated HELLO handshake,
and the JPEG strip stream (audio reserved) — is specified in
[`docs/protocol.md`](docs/protocol.md); that doc is the contract between the
agent and the Tab5 side (`embedded_adb`/`adb` + the `agent_link` component).

## Layout

```
src/com/tab5adb/agent/Server.java   # entry point (main); LocalServerSocket loop
build.sh                            # javac + d8  -> build/tab5adb-agent.jar
run.sh                              # adb push + app_process (dev loop)
```

Build artifacts go to `build/` (gitignored).

## Build & verify

Everything runs through the Nix dev shell (adb, JDK, android.jar, d8 all come
from the flake — see the repo root). With a phone connected and USB debugging
authorized:

```sh
nix develop -c android-agent/build.sh      # -> build/tab5adb-agent.jar
```

**Primary verification = the headless Tab5-side harness** (the real
`embedded_adb`/`adb` + `agent_link` stack over libusb, no GUI). It pushes the jar,
launches `app_process`, opens `localabstract:tab5adb-agent`, and runs the HELLO
handshake against the phone — run it with
`nix develop -c components/agent_link/test/run.sh` (the runner builds the host
stack, runs `adb kill-server`, and launches the test). The test approach (headless,
no LVGL, real Tab5-side stack) is documented in [`docs/testing.md`](docs/testing.md).

`run.sh` still launches the agent standalone for manual poking, but the agent now
speaks the binary HELLO protocol (it sends a HELLO frame and waits for the Tab5
response), so a plain `nc`/`recv` over `adb forward` no longer reads a text banner
— it is only a **debug fallback** to check whether a problem is in the agent or in
the Tab5-side stream open:

```sh
nix develop -c android-agent/run.sh        # push + app_process, stays in foreground
# clean up:  adb shell pkill -f com.tab5adb.agent.Server
```

Verified against a real Android device (Android 14, API 34).

## Build notes

- **Java, not Kotlin** — a minimal Java dex is a few KB with no `kotlin-stdlib`
  to bundle, which keeps the artifact small enough to embed in the Tab5 firmware
  (gzip + `xxd` → C array). `scrcpy` is also Java, so its capture code reads as a
  reference.
- `javac -source 8 -target 8 -classpath android.jar` then `d8 --release` into a
  jar (the jar holds `classes.dex`; `app_process` loads it via `CLASSPATH`).
- Hidden APIs (for capture, later) are reached by **reflection** for now, so the
  build needs only the public `android.jar`.
