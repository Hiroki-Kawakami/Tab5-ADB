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

Phase 1 skeleton: the server listens on `localabstract:tab5adb-agent` and serves
a banner — enough to verify the build + `app_process` launch + socket path end to
end with standard adb. Screen capture / offload services and the wire protocol
come later.

## Layout

```
src/com/tab5adb/agent/Server.java   # entry point (main); LocalServerSocket loop
build.sh                            # javac + d8  -> build/tab5adb-agent.jar
run.sh                              # adb push + app_process (dev loop)
```

Build artifacts go to `build/` (gitignored).

## Build & run (dev loop, standard adb)

Everything runs through the Nix dev shell (adb, JDK, android.jar, d8 all come
from the flake — see the repo root). With a phone connected and USB debugging
authorized:

```sh
nix develop -c android-agent/build.sh      # -> build/tab5adb-agent.jar
nix develop -c android-agent/run.sh        # push + app_process, stays in foreground
```

In another shell, reach the socket from the PC and read the banner:

```sh
nix develop -c adb forward tcp:8080 localabstract:tab5adb-agent
python3 -c 'import socket; print(socket.create_connection(("localhost",8080),3).recv(4096).decode())'
# tab5adb-agent v0
# model=real Android device
# android=14 (sdk 34)
```

Stop and clean up:

```sh
nix develop -c sh -c 'adb forward --remove tcp:8080; adb shell pkill -f com.tab5adb.agent.Server'
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
