# esp-devkit BSP integration

`bsp_*` is the cross-platform hardware seam used directly by `app/` on both
targets. Its implementation now lives in the `esp-devkit/bsp/` submodule; this
repository does not carry a fork or a second porting layer. Reusable board or
simulator changes belong in esp-devkit first, followed by a submodule-pointer
update here.

The device and simulator select Tab5 with `CONFIG_BSP_BOARD_TAB5=y` in their
respective `sdkconfig.defaults` files. Both select the same model-agnostic
public API in `esp-devkit/bsp/inc/bsp.h`, with device and SDL providers chosen
by the build.

The Tab5 implementation is under `esp-devkit/bsp/boards/tab5/`:

- `board.cmake` declares the board's device drivers and simulator source.
- `tab5.c` brings up I2C, the IO expanders, panel/touch, SD and audio. Display
  failure is fatal; unavailable SD or audio leaves those optional capabilities
  disabled without blocking the UI.
- `tab5_sim.c` supplies the same BSP surface using the shared SDL panel/audio
  backends. Generic simulator SD redirection is linked by the BSP component.

## Display and power

`DisplayManager` (`app/display_manager.{hpp,cpp}`) owns the LVGL display,
framebuffer presentation, touch indev and mirror overlay compositor. App code
uses it instead of writing panel state directly, while the manager delegates
framebuffer allocation, format and flushing to `bsp_display_*`.

The panel pixel format is fixed during `bsp_init`. The app requests three
buffers in the persisted RGB565/RGB888 format so the mirror decode path can
rotate buffers without waiting for the current scan-out. Normal LVGL rendering
and overlay composition use that same BSP-owned framebuffer set.

USB VBUS is the generic `BSP_POWER_SWITCH_USB5V` power switch. `adb_app()`
applies the stored preference after `bsp_init`, and its USB-host reset hook
cycles that switch to force a physical re-enumeration edge. Unsupported boards
return `ESP_ERR_NOT_SUPPORTED`; no Tab5 IO-expander detail leaks into the ADB
component. Settings restart uses `bsp_power_restart()`.

## SD card

SD is part of esp-devkit's public `bsp.h`; there is no project-local
`bsp_sd.h`. The app mounts on demand with `bsp_sd_mount("/sd", ...)`, uses
ordinary POSIX file I/O below that mount point, and calls `bsp_sd_unmount()`
after a failed scan so Refresh can retry after card insertion.

- Device: the Tab5 provider is a four-bit SDMMC implementation using slot 0 and
  on-chip LDO channel 4. Slot 1 remains reserved for the C6 esp-hosted Wi-Fi
  link. With esp-hosted enabled, the SD driver borrows its initialized SDMMC
  host while still owning slot 0; without esp-hosted it manages the host itself.
  The provider owns the power handle across a mounted session.
- Simulator: `esp-devkit/bsp/simulator/sd_redirect.c` maps `/sd` onto
  `SIMULATOR_SDCARD_PATH`, defaulting to `simulator/sdcard/`. It redirects the
  POSIX calls used by the app, so browser and file-transfer logic exercise the
  same paths as the device build.

Plain `fread` is slow on the device path. APK push reads in 16 KiB chunks with
`read()` into a cache-aligned buffer. Long file names require
`CONFIG_FATFS_LFN_HEAP`, retained in `esp32p4/sdkconfig.defaults`.

## Touch dispatch

The shared BSP dispatch task owns touch sampling and delivers display-space
snapshots through `bsp_touch_set_event_cb`. `DisplayManager::init()` registers
one callback; it caches pointer 0 for LVGL and forwards the complete sample to
the active weak `TouchListener`. The callback runs off the LVGL thread, so
listeners must synchronize their state and marshal any widget access.

Each `bsp_touch_point_t` includes the controller track id, allowing the mirror
to correlate multiple fingers and emit Android pointer transitions without
synthesizing ids. `consume_overlay_touch()` masks an in-progress reveal gesture
from the LVGL indev until all fingers lift, preventing that same press from
activating a newly exposed overlay button.

Keeping touch polling in the BSP matters because button/audio routing and touch
share one board dispatch policy. A project-owned touch task would duplicate
polling, bypass board scheduling and make reusable BSP callbacks ineffective.

## Audio

The capability-based `bsp_audio_*` surface represents no-audio, tone, PCM,
speaker and headphone variants without Tab5-specific branching in the app.
The shared dispatch owns volume/mute, speaker routing and DSP policy; providers
only implement low-level codec or simulator operations.

`bsp_init` must produce no audible signal. A stream opens with
`bsp_audio_open()`, audio writes are naturally paced, and route/volume changes
use the shared DSP fade. Under `SIMULATOR_HEADLESS`, the SDL provider becomes a
silent real-time-paced sink so scripted verification still exercises producer
timing.
