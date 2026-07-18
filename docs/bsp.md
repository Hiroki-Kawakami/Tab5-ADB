# `m5stack-bsp` — board support

`bsp_*` **is** the cross-platform hardware seam: `app/` calls `bsp_*` directly
on both targets, there is no separate porting layer. The component is shared
(`components/m5stack-bsp/`) and builds device drivers or the SDL simulator
backend from its own `ESP_PLATFORM`-branched `CMakeLists.txt` (see
[architecture.md](architecture.md#components-are-self-describing-idf_component_register)).

Structured so non-Tab5 M5Stack models can be added later without reworking the
drivers:

- **Public API (`inc/bsp.h`)** — model-agnostic: `bsp_init`/`bsp_restart`,
  `bsp_display_*`, `bsp_touch_*` (touch points are the BSP's own
  `bsp_touch_point_t` in `bsp_types.h` — no `esp_lcd_touch` type leaks into the
  public API). The dispatch (`src/bsp_display.c`, `src/bsp_touch.c`) is
  implemented **once**, holding the active provider and dispatching through its
  vtable — a board never re-implements this glue.
- **Internal driver interfaces (`inc_private/bsp_display.h`, `bsp_touch.h`)** —
  struct-inheritance vtables (esp_lcd style): a driver embeds `bsp_display_t`/
  `bsp_touch_t` as its **first** struct member and returns `&state->base` from a
  `*_create()` factory. Host-side framebuffers (`get_framebuffers`+`flush`) and
  backlight (`set_brightness`) are **optional** (NULL when the panel lacks the
  capability) — so an EPD or SPI-with-GRAM panel fits without the MIPI
  framebuffer-swap model baked into the contract; today only the framebuffer
  path is wired and the app assumes it.
- **Device drivers (`devices/`)** — reusable chip drivers (ili9881c, st7123,
  gt911, es8388, pi4io), each a `bsp_display`/`bsp_touch` provider. Include only
  `bsp_display.h`/`bsp_touch.h` (+`bsp_types.h`), never `bsp_private.h`.
- **Simulator backend (`simulator/`)** — the host-side analogue of `devices/`: a
  reusable SDL backend (`sdl_backend.cpp`) that turns one SDL window into a
  `bsp_display` + `bsp_touch` provider, parameterized per model via
  `sdl_backend_config_t` (window title, geometry, pixel format, buffer count,
  on-screen scale). SDL is main-thread-only on macOS, so
  `sdl_backend_pump_input()` (called from the main loop) is the only thing that
  touches SDL for input — it drains events and writes a mutex-guarded touch
  snapshot that `bsp_touch_read` (called from the background touch task, see
  below) just copies.
- **Boards (`boards/<model>/`)** — `bsp_init()`/`bsp_restart()`, the only
  per-model pieces: a device variant (`<model>.c`) and a simulator variant
  (`<model>_sim.c`), selected by the `ESP_PLATFORM` branch. Tab5's `tab5.c`
  resolves the panel generation (ST7123 vs ILI9881C/GT911) by I2C probe +
  plain `if` (board-internal, not abstracted further) and wires the matching
  providers.

Everything in the BSP is **C** on both targets (touch/display; audio is also
C). In the vtable header `inc_private/bsp_touch.h`, `bsp_touch_config_t`
(device bus/GPIO wiring) is `#ifdef ESP_PLATFORM` so the host build never pulls
in `driver/*`.

## Audio (`bsp_audio_*`)

Capability-based so every M5Stack audio variant (no audio / buzzer-only /
speaker-only / speaker+HP) fits one API: `bsp_audio_get_caps()` returns
`BSP_AUDIO_CAP_{PCM,TONE,SPEAKER,HEADPHONE}` bits; unsupported calls return
`ESP_ERR_NOT_SUPPORTED` (no provider = caps 0).

Design intent worth knowing before touching this code:
- **`bsp_init` must produce no signal.** Providers register *closed* — the
  stream format is `bsp_audio_open()`'s argument, not part of `bsp_config_t`
  (which only carries `audio.dsp_mode`/`audio.speaker_mode`); write/reconfig/
  close before open return `ESP_ERR_INVALID_STATE`.
- **The shared dispatch (`src/bsp_audio.c`) owns all policy** — providers only
  implement low-level vtable ops (open/close/write/set_hw_volume/set_hw_mute/
  set_speaker_enabled/headphone_inserted/get_dsp_profile/tone; each optional =
  NULL). Policy = the volume curve (linear-in-dB, delivered as a fading SW
  gain — **never a HW step**, that's the click-free contract), mute, the
  speaker route ON/AUTO/OFF policy + headphone-insert poll/callback, and the
  DSP voicing mode.
- **Click-free sequencing contract** (stated in `inc_private/bsp_audio.h`): amp
  state changes only while the DAC is settled silence; audible amplitude
  changes only via the SW fade. The first `open` "arms" the amp (silent gain →
  codec unmute at max HW volume → ~50ms analog settle → amp on); `close`
  HW-mutes before the clocks stop but **keeps the amp** (so its transient isn't
  re-paid every open); `bsp_audio_quiesce()` (mute + amp off) is the
  `bsp_restart()` path. Device pop-noise behavior and the close-path settle
  delays still need a real-HW flash check.
- **DSP voicing modes** (`bsp_audio_dsp_mode_t`, zero-init = Auto): *Auto* pulls
  the board's per-headphone/per-rate voicing from the provider's
  `get_dsp_profile()` hook and **re-applies it on HP insert/remove** (so app
  edits to the DSP get overwritten in this mode — intentional, it's the "just
  sound right" default); *Manual* starts flat and leaves the app in control via
  `bsp_audio_dsp()`; *Disable* skips DSP entirely, volume falls back to the HW
  codec (clicky).
- EQ/post-processing itself is the separate, board-independent **`audio_dsp`**
  module (`inc/audio_dsp.h`+`src/audio_dsp.c`): a fixed chain
  EQ (cascaded RBJ biquads) → gain (the click-free volume primitive, fades
  continue seamlessly across `process()` chunk boundaries) → stereo→mono mix.
  Stage capacity auto-grows via `set_biquads`, but beyond 8 stages `process()`
  falls back to holding the lock instead of stack-snapshotting (the grow
  realloc would dangle a direct pointer taken earlier).
- Simulator provider (`simulator/sdl_audio.c`) **backpressures at ~100ms
  queued** to mimic the blocking I2S DMA write, and degrades to a silent
  **null sink with the same real-time pacing** under `SIMULATOR_HEADLESS` (or no
  host audio device) — so verify runs still exercise producer timing even
  without sound.

## SD card (`inc/bsp_sd.h`)

Mount/unmount only — once mounted, app code uses plain POSIX file I/O under the
mount point on both targets, no further BSP seam. Mount on demand; a failed
scan unmounts so the next Refresh re-mounts (no hot-plug detection).

- Device: the TF slot **must** be SDMMC slot 0 (see
  [gotchas.md](gotchas.md#wi-fi--esp-hosted-componentswifi-esp32p4sdkconfigdefaults)
  for why — slot 1 is the C6 Wi-Fi SDIO link). Card power comes from the
  on-chip LDO channel 4, kept acquired across remounts.
- **Read-performance rule:** plain `fread` is slow on this path — read with
  unbuffered `read()` in 16KB chunks into a `MALLOC_CAP_CACHE_ALIGNED` buffer
  instead (what the APK-install push source does). Long file names need
  `CONFIG_FATFS_LFN_HEAP` (the default `LFN_NONE` truncates to 8.3).
- Simulator (`simulator/sd_redirect.c`): "mount" maps the mount point onto a
  host directory (`SIMULATOR_SDCARD_PATH`, default `simulator/sdcard/`) by
  defining `open`/`fopen`/`opendir`/`stat`/`rename`/`unlink` in the executable —
  statically-linked calls resolve there, and the real libc is reached via
  `dlsym(RTLD_NEXT, ...)`.

## DisplayManager touch input

`DisplayManager` (`app/display_manager.{hpp,cpp}`) owns the touch indev, but
**the hardware read is decoupled from the LVGL render loop**: a dedicated
FreeRTOS touch task does all `bsp_touch_read`, so panel refresh / JPEG decode
latency never delays input.

- **Interrupt-gated + idle-stop:** the task blocks on
  `bsp_touch_wait_interrupt()` (device = the GT911/ST7123 INT semaphore; sim = a
  short delay), then polls at `touch_poll_hz_` (default 60Hz), and after 3
  consecutive empty reads goes back to waiting for the next interrupt. This
  needs the touch controller's INT line enabled at `bsp_init`
  (`config.touch.interrupt = true`) — the semaphore is only created when that
  flag is set, so any caller of `bsp_touch_wait_interrupt()` must ensure it's
  on, or the driver asserts on a NULL semaphore.
- **Push, not poll, for multi-touch.** Each sample both feeds LVGL's
  single-tap indev (id 0 only) *and* pushes all contemporaneous points to a
  `DisplayManager::TouchListener` (`on_touch(pts, count)`) that a feature
  registers with `set_touch_listener(weak_ptr)` — held weakly, fires **on the
  touch task thread** (the listener marshals to LVGL itself if it needs to).
  This is what lets the mirror screen's touch passthrough and corner-swipe
  reveal detection see genuine multi-touch without polling.
- Each `bsp_touch_point_t` carries the controller's pointer track id (`.id`;
  GT911/ST7123 forward `esp_lcd_touch`'s `track_id`, the single-point sim
  reports 0), so a listener can correlate fingers across samples without
  synthesizing ids itself.
- **Swipe-reveal masking:** when a corner-swipe makes a hidden overlay visible,
  the same in-flight press would otherwise land as a tap on the freshly-shown
  button underneath it. `consume_overlay_touch()` masks the press from the
  indev until the finger lifts — callers must invoke it *before* making the
  overlay visible, to close the visible-without-mask window.
