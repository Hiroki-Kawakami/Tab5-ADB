# Screen mirroring & offload (`tab5adb-agent`, `agent_link`, `AgentClient`)

## tab5adb-agent

`android-agent/` — the Android-side companion, side-loaded onto the phone.
Its main purpose is **screen mirroring**; it also hosts offload work the Tab5
can't do itself (mirroring is one service among several). It is a
**scrcpy-style server, not an APK**: a plain Java program dexed into a jar,
pushed to `/data/local/tmp`, and launched with `app_process` so it runs with
shell uid (2000) and reaches hidden Android APIs (display capture, input
injection, media session binder) with no permission dialog. It listens on the
abstract socket `localabstract:tab5adb-agent`, reached over the embedded ADB.
See `android-agent/README.md` for the process/build overview and
`android-agent/docs/protocol.md` for the full wire format (this page assumes
that spec and covers only the *why*, not the *what*).

The jar is embedded in the firmware as a C array (`app/agent/agent_jar.{h,c}`)
so the push+launch path needs no host file — **regenerate it after every
`android-agent/src/` Java change** (see
[development.md](development.md#android-agent--android-agent)).

`app_process` gives the agent none of Zygote's app scaffolding, so several
ordinary Android APIs fail in agent-specific ways — see
[gotchas.md](gotchas.md#android-agent--app_process-android-agentsrc).

## Mirror pipeline design

**Geometry (rotate → scale-fit/fill → letterbox) is GPU-offloaded on the
agent**, not done per-frame on the Tab5, via the compositor: primarily the
hidden static `DisplayManager.createVirtualDisplay(name, w, h, displayId=0,
surface)` (the only path that works on Android 14/15, where
`SurfaceControl.createDisplay` was removed), falling back to
`SurfaceControl.createDisplay` + `setDisplayProjection` on older Android. Either
way `acquire()` needs no CPU readback or Bitmap copy — the frame lands
upright/scaled/letterboxed already. This is what got the mirror from a
15fps-capped ~23fps CPU path to ~33-37fps.

**Physical-orientation lock:** the primary capture path naturally follows
display 0's *logical* rotation, which would rotate+shrink-letterbox the Tab5
every time the phone turns. Instead `ScreenCapture` builds the reader sized to
the panel *oriented to the current rotation* (so the GPU scale still fills it
with no orientation-mismatch letterbox), then counter-rotates the acquired
frame back to the natural-orientation `targetW×targetH` — so a landscape app
shows sideways and full-size (turn the Tab5 to view it upright), never
shrunk. This only costs a CPU rotation on an actually-rotated frame; at the
common `ROTATION_0` it's a no-op.

**The agent always emits a full panel-width (720px) frame**, even though the
requested viewer surface may be smaller — scale-fit + black letterbox are
baked into that full-width frame agent-side. This is not a mirror-specific
choice: it's forced by the **P4 JPEG 2D-DMA's inability to stride** a narrower
picture into a wider destination buffer (see
[gotchas.md](gotchas.md#display--jpeg)) — full-width strips are what let the
Tab5 decode each strip straight into its framebuffer row band with no extra
copy. `MIRROR_START`'s `target_w/h` therefore describes the *viewer surface*
the agent scales into, not necessarily the panel; small previews
(`AgentPreview`, `ScreencapPreview`-equivalent) instead use `scale_mode=aspect`
(the agent picks the output size to the source's natural aspect) with
`split_count=1` (one whole JPEG per frame, no strip banding, no 16px-alignment
requirement) — that's the wire basis for the small live preview on
`ADBDeviceScreen`.

**`scale_mode=adapt`** is the opposite knob from `aspect`: instead of resizing
the *output*, the agent `wm size`s the *source* app to the viewer's aspect
(`IWindowManager.setForcedDisplaySize`) so a plain fit fills the panel with no
letterbox or crop — and it **must not** be used for a small preview, since it
resizes the real phone. The agent owns applying *and restoring* this override
entirely on its own side (on MIRROR_STOP / a switch-away MIRROR_START /
disconnect / JVM shutdown signal) so a yanked USB cable can never strand the
phone at the override — the Tab5 sends no `wm size` itself for this mode.

**A `MIRROR_START` arriving mid-stream reconfigures the live session in
place** (no `stop_mirror` round trip) — the send loop just breaks and restarts
with the new params. This is what lets the mirror's DispMode toggle
(Fit→Fill→Adapt) and quality/fps re-tuning apply without a stop+start race that
could otherwise hang the agent.

## Media channel

The now-playing media card on `ADBDeviceScreen` prefers the agent's **MEDIA
channel** over the agent-free `dumpsys media_session` fallback (used only in
Limited mode — see [ui.md](ui.md)) because it gets three things the exec-based
path can't: **push** delivery (no polling), **album art**, and **any-script
title/artist text** — the agent renders the text to a bitmap agent-side
(`TextRender.java`), so the Tab5 itself needs no CJK font. A change in
`content_token` (a track-identity hash) triggers one `GET_MEDIA_RENDER`
fetch; a state-only change (play↔pause) just flips the transport glyph.
Transport taps go over `MEDIA_CONTROL` (the *live* session's
`MediaController`, low latency) rather than a shell `cmd media_session
dispatch`.

## Input injection

Keys/touch go over a dedicated **fire-and-forget `TYPE=INPUT` channel** on
`agent_link`, deliberately **not** `adb shell input` and **not** the
request/response `CONTROL_REQUEST` path — high-frequency touch must never wait
on a round trip. The agent injects via the hidden
`InputManager.injectInputEvent` (the scrcpy technique).

Touch uses timestamped, full multi-pointer snapshots. `ADBMirroringScreen` only
classifies points as pass/reveal/overlay and sends the complete pass set; the
agent owns the previous set and derives Android DOWN/MOVE/UP actions. Repeated
polls at identical coordinates produce no `MotionEvent`, and the Tab5 sample
timestamp becomes Android `eventTime`, preserving velocity history when network
delivery is bursty.

USB sends each changed snapshot in its own `kInputTouchSnapshot` frame. TCP/Wi-Fi
uses `kInputTouchSnapshotBatch` because ADB's per-`A_WRTE`/`A_OKAY` stop-and-wait
can stall the whole mirror. Each batched snapshot retains its own timestamp and
is self-contained, so bounding a stalled batch by dropping its oldest snapshot
cannot corrupt pointer state.

## Mirror rendering (Tab5 side, `ADBMirroringScreen`)

**Rendering bypasses LVGL entirely** — the draw cost of compositing UI over a
video stream is exactly what this avoids — and receive/decode run on
**separate threads** so the blocking HW-JPEG decode never stalls the adb
reader thread (which would otherwise cap fps by delaying the per-`A_WRTE`/
`A_OKAY` flow control that gates the next USB IN transfer):
- **producer = the adb reader thread**: copies each strip's JPEG bytes into a
  PSRAM frame slot and publishes the finished frame, returning immediately so
  the link stays flowing.
- **consumer = a private decode task**: HW-JPEG-decodes each strip straight
  into a bsp framebuffer and presents it.

Slots move `free → producer fills → ready (cap-1, latest-frame-wins) →
consumer decodes → free`, so a slow decoder **drops whole frames** instead of
stalling the reader. The three bsp framebuffers are used as a **triple
buffer** (decode into the one two frames behind the displayed one) so the
decode task never blocks on panel scan-out before reusing a buffer.

The control overlay (Back/Home/Recents/Vol/Power/OpMode/DispMode/Hide/End) is
a small LVGL display composited at flush time by `DisplayManager`, anchored to
whichever panel corner becomes the viewer's bottom-left after accounting for
the *source* device's rotation (`agent_link`'s ORIENTATION event) — see
`ADBMirroringScreen`'s `view_rot()` for the corner/angle mapping (verified
against real-hardware handedness; flip it if a different device shows the
opposite handedness).

## Audio (`app::AgentAudio`)

The audio analogue of the video pipeline. USB keeps the low-latency 48 kHz
stereo PCM path; TCP/Wi-Fi requests 96 kbps VBR Opus in 20 ms raw packets.
`on_audio_data` fires on the adb reader thread and only copies the codec unit
into PSRAM, so audio cannot stall video flow control. On Wi-Fi, `opus_decode`
(Core 0, priority 4) converts packets into a decoded PCM-frame queue while
`agent_audio` (Core 1, priority 4) independently drains PCM to
`bsp_audio_write`. This keeps decoder stalls out of the I2S pacing task. Playback
starts and resumes after five decoded frames (100 ms); both queues cap burst
backlog at one second. The pipeline is created/destroyed with the mirror screen
and used only in Tab5Only mode.

## `agent_link` — the Tab5-side wire client

`components/agent_link/` is the Tab5 end of the protocol
(`android-agent/docs/protocol.md`): one TYPE-multiplexed ADB stream carrying
control + video + audio. **Dependency direction:** `agent_link` (app-specific)
→ `adb` (generic; wire engine private) — the generic `adb::Client` never knows
`agent_link` exists (see [adb.md](adb.md#adbs-generic-raw-stream--why-it-exists)).
`Link::open()` opens `adb::Client::open_stream("localabstract:tab5adb-agent",
…)` and layers framing on top; unlike `Sync`'s worker+pipe, the parser is
**reactive on the reader thread** (the agent mostly pushes to the Tab5, so
there's little to block on).

**Listeners are split per channel**, mirroring the wire's TYPE multiplexing, so
independent consumers attach to just their slice without coordinating:
`LinkLifecycleListener` (connection state — owned by `AgentClient`),
`VideoListener` (mirror strips + orientation — owned by whichever screen is
showing video), `MediaListener`, `AudioListener`. All held as `weak_ptr`,
Shell/Sync-style. A generic `Link::request(cmd, …)` handles any one-shot
`CONTROL_REQUEST` (app list/icon, media info/render) with req_id correlation
and lazy timeout sweeps, so adding a new control command doesn't need new
plumbing in `Link` itself.

## `app::AgentClient` — agent process lifecycle

`app/agent_client.{hpp,cpp}` is the app-global owner of the tab5adb-agent
*process* lifecycle (pkill stale agent → push jar → launch `app_process` →
retry HELLO), decoupled from any one screen so multiple features share one
agent session. It deliberately **does not forward per-feature protocol** —
features drive `agent_link::Link` directly via `AgentClient::link()` — so this
class can't grow into a thin `Link` wrapper as new agent features are added.

- **Mode is the app-wide feature gate.** The connect flow calls
  `ensure_connected` eagerly right after adb reaches Online (before pushing
  `ADBDeviceScreen`), so every screen reads an already-settled `mode()`:
  `Normal` (agent up — mirror, AgentPreview, app icons, media) vs `Limited`
  (agent-independent features only). A later mid-session link drop does **not**
  demote the mode — features just lazily `ensure_connected` again on next use.
- **Bring-up is fail-fast**, since it gates the connect UX: an overall ~8s
  HELLO-retry deadline, and it bails immediately if the agent's launch shell
  closes early (matched against the *current* shell session, since a dropped
  previous session's shell can deliver a stale close callback).
- **`stop_mirror()`/clearing a feature's listener never drops the link** — only
  `on_link_close` (agent died) or `on_adb_disconnected()` (adb link closed)
  tear the session down. This is the whole point of `AgentClient` owning the
  link instead of the mirror screen: re-entering the mirror resumes instantly.
- The `app::agent_client()` singleton is a **deliberately-leaked** heap
  `shared_ptr` — other statics (e.g. `ScreenManager` tearing down a live
  mirror screen) call into it from their own destructors at process exit, and
  cross-TU static destruction order is unspecified; a normal Meyers singleton
  raced a destroyed mutex once. The device firmware never exits, so nothing
  actually leaks.
