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

On each connection the server runs the protocol's **HELLO handshake** (link
establishment only — proto / version / capability), then waits for the Tab5 to
send **MIRROR_START** and **streams the screen as JPEG strips** (Phase 2, done):
it captures via hidden display APIs into a fixed 720×1280 `ImageReader` and streams
horizontal-strip → JPEG (YUV420 q60). A **dedicated control-reader thread** reads
inbound frames concurrently with the JPEG send loop, so a Tab5 **MIRROR_STOP** stops
the stream and returns the agent to READY **without closing the socket** — a later
MIRROR_START resumes on the same link (this is what lets the Tab5 keep the agent
connected while leaving the mirror feature). The **rotate → scale → black letterbox**
geometry is GPU-offloaded — `ScreenCapture` tries the static hidden
`DisplayManager.createVirtualDisplay` first (mirrors display 0 into the reader,
aspect-fit; the path that works on **Android 14/15** where
`SurfaceControl.createDisplay` was removed), and falls back on older Android to
`SurfaceControl.createDisplay` + `setDisplayProjection` (explicit fit/fill rect math
in `Projection`). Either way the compositor does it — no CPU readback/copies — so
the pipeline only splits + encodes. **fit/fill** (`MIRROR_START scale_mode`) work on
both paths: the fallback uses a negative dest rect; the always-fit primary path gets
fill by oversizing the reader to the cover size (`Projection.fillCover`) and cropping
the centered panel out as the strip read-origin (no extra copy). A `--test-pattern` mode streams a deterministic
frame through the CPU geometry (`FramePipeline`) instead (for headless
verification without the capture APIs). No artificial FPS cap (capture-rate
driven). Verified against a real Android device by the Tab5-side harnesses `test_hello.cpp`
(HELLO) and `test_mirror.cpp` (strip mirror, incl. a real-capture smoke test), and
the host-JVM `test/ProjectionTest` for the projection math.

The wire protocol — single-socket framing, the agent-initiated HELLO (link-only)
handshake, the Tab5-initiated MIRROR_START / MIRROR_STOP, and the JPEG strip stream (audio
reserved) — is specified in [`docs/protocol.md`](docs/protocol.md); that doc is
the contract between the agent and the Tab5 side (`embedded_adb`/`adb` + the
`agent_link` component).

## Layout

```
src/com/tab5adb/agent/
  Server.java         # entry point (main); sockets, HELLO + MIRROR_START, stream loop
  FramePipeline.java  # strip + JPEG (stripsOf); CPU rotate/scale geometry for --test-pattern (§5.1)
  Projection.java     # pure GPU-projection math (rotate/scale-fit/center); host-JVM testable
  TestPattern.java    # deterministic source frame for --test-pattern verification
  ScreenCapture.java  # real capture -> 720x1280 ImageReader: DisplayManager.createVirtualDisplay (A14/15) | SurfaceControl projection fallback
test/                 # host-JVM unit test (ProjectionTest) + run.sh — no phone
build.sh              # javac + d8  -> build/tab5adb-agent.jar
run.sh                # adb push + app_process (dev loop)
```

Build artifacts go to `build/` (gitignored).

## Build & verify

Everything runs through the Nix dev shell (adb, JDK, android.jar, d8 all come
from the flake — see the repo root). With a phone connected and USB debugging
authorized:

```sh
nix develop -c android-agent/build.sh      # -> build/tab5adb-agent.jar
```

**Primary verification = the headless Tab5-side harnesses** (the real
`embedded_adb`/`adb` + `agent_link` stack over libusb, no GUI). They push the jar,
launch `app_process`, open `localabstract:tab5adb-agent`, and drive the protocol
against the phone:

```sh
nix develop -c components/agent_link/test/run.sh                       # HELLO (test_hello)
nix develop -c sh -c 'TEST=test_mirror components/agent_link/test/run.sh'   # JPEG strip mirror
#   TAB5ADB_REAL=1 on test_mirror smoke-tests real SurfaceControl capture
```

`test_mirror` decodes the received strips with host libjpeg and asserts
framing / 16-alignment / per-frame tiling (it also writes `build/mirror_frame.ppm`
for eyeballing). The test approach (headless, no LVGL, real Tab5-side stack) is
documented in [`docs/testing.md`](docs/testing.md).

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
- Hidden APIs (`SurfaceControl` display capture, `DisplayManagerGlobal`) are
  reached by **reflection**, so the build needs only the public `android.jar`.
- No `Canvas.drawText` — a bare `app_process` has no default `Typeface`, so text
  drawing aborts; the test pattern uses coloured shapes instead.
