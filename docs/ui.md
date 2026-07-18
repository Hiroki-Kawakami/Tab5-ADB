# App UI (`app/`)

`app/` is a single shared component: LVGL screens + the pure-logic modules they
call into (`device_info`, `apk_info`, `media_session`, `sysclock`, `png_decode`
— all host-tested in `app/test/`, no LVGL/adb deps). This page covers the
cross-cutting UI patterns; screen-by-screen behavior is best read from the
source (one `.cpp`/`.hpp` pair per screen).

## Screen lifecycle & threading — the pattern every screen follows

Every screen that talks to `adb`/`agent_link`/`wifi` (i.e. registers itself as
a listener) follows the same shape, because those libraries fire callbacks off
the LVGL thread (see `components/adb/README.md`'s threading rule):

- The screen **is** the listener interface (`adb::ShellListener`,
  `adb::SyncListener`, `agent_link::VideoListener`, `wifi::Listener`, …) and
  registers itself as a `weak_ptr` aliasing its own `shared_from_this()` — so
  there is no explicit `detach()`; dropping the screen's `shared_ptr` (on pop)
  detaches automatically, and an in-flight callback's `lock()` just no-ops if
  the screen is already gone.
- Every callback that must touch LVGL captures `self = shared_from_this()` and
  marshals via `lv_async_call`, and every marshalled lambda checks
  `Screen::exited()` before touching any UI object (the screen may have been
  popped between the callback firing and the async call running).
- High-frequency data streams (shell stdout, logcat lines, sync progress) never
  get one `lv_async_call` per chunk — `lv_async_call` runs queued callbacks
  **LIFO** (see [gotchas.md](gotchas.md#lvgl-componentslvgl-app)), so a stream
  buffers into a capped FIFO and coalesces to **one** flush per frame.
- A monotonically-increasing generation counter (`nav_gen_`/`load_gen_`,
  LVGL-thread-only) is the standard way to drop a stale async completion when
  the user navigates/re-lists faster than the device responds — every screen
  with a list-refresh or directory-navigation flow uses this, not just the file
  browser.

`ScreenManager` (`components/screen_manager/`) owns the screen stack; pop/load
loads the next screen **before** freeing the leaving one, deferred via
`lv_async_call` (see [gotchas.md](gotchas.md#lvgl-componentslvgl-app) — deleting
the still-active screen inside its own event handler segfaults).

## Navigation map

```
HomeScreen                          USB / Wireless(TCP) connect, About, Settings, Files(SD, adb-less)
├─ AboutScreen                      app identity, links (QR modals), acknowledgements
├─ SettingsScreen → WiFiScreen      Wi-Fi on/off, scan/connect (wifi::Manager — see components/wifi/)
├─ SDFileBrowserScreen               local SD card, adb-less (browse/pick/pick-dir modes)
└─ ADBDeviceScreen                   summary header, live preview, nav row, media card, tool grid
   ├─ ADBDeviceInfoScreen → ADBMetricsScreen   device detail / live CPU+mem
   ├─ ADBShellScreen                 VT100 terminal (term_emu + term_view/term_keyboard)
   ├─ ADBFileManagerScreen           storage picker → ADBFileBrowserScreen (Android) / SDFileBrowserScreen
   │   └─ {Image,Apk,generic} preview screens   file_preview.cpp registry, keyed by extension
   ├─ ADBAppManagerScreen → ADBAppDetailScreen  installed apps (agent GET_APP_LIST or pm fallback)
   ├─ ADBLogcatScreen                 live filtered logcat, PSRAM ring buffer
   ├─ ADBScreenshotScreen             one-shot screencap capture/save
   └─ ADBMirroringScreen              live mirror — see agent.md
```

Tapping the device preview column on `ADBDeviceScreen` is the mirror entry
point in Normal mode (Limited mode explains why it can't, via a modal). See
[agent.md](agent.md) for the mirror rendering pipeline, and
`components/wifi/README.md`/`docs/wifi.md` for the `wifi::Manager` API the
`WiFiScreen`/HomeScreen Wi-Fi card drive.

`HomeScreen`'s USB Connect button is tapped by fixed coordinates in several
simverify scripts — if you move it, update those scripts too.

## Agent-mode-dependent screens

Several screens branch on `app::agent_client().mode()` (see
[agent.md](agent.md#appagentclient--agent-process-lifecycle)) between a richer
**Normal**-mode path (push-driven, over `agent_link`) and a **Limited**-mode
fallback (agent-free, plain `adb exec`): the device-screen live preview
(`AgentPreview` vs `ScreencapPreview`), the now-playing media card (agent MEDIA
channel vs `dumpsys media_session` polling — CJK track titles render as tofu
in Limited mode, since only the agent can render text to a bitmap), and the
app manager's icon fetch (`GET_APP_ICON` vs package-name-only `pm list`). Both
paths stay wired even after the agent comes up, since a mid-session agent link
drop must degrade gracefully without an app restart.

## Recycled list widgets

Long lists (installed apps, logcat lines) use a fixed pool of row widgets
**rebound** to the visible scroll window instead of building one LVGL object
per item — a per-package/per-line object churn was visibly slow on-device even
for moderate list sizes. The pattern: a pool sized to `viewport height + a few
rows`, an invisible `extent_` child sized to `count * row_height` to define the
scroll range, and a `LV_EVENT_SCROLL` handler that recomputes which pool row
maps to which data index and just updates its labels/icon/y-position. Any new
long list should follow this rather than building N widgets.

## File preview & transfer jobs

`app/file_preview.{hpp,cpp}` is an extension-keyed registry:
`make_file_preview(FileRef)` never returns null — an unmatched extension gets
the generic info-card preview. Per-type screens (image, APK) share the same
chrome helpers (`preview_chrome`/`preview_header`/`preview_info_row`/
`preview_action`) so they look alike without a common base class.

`app/file_transfer.{hpp,cpp}` (`pull_to_sd`/`push_to_android`/`install_apk`) is
deliberately **screen-agnostic** — it takes a `FileRef`, not a screen pointer —
so any future entry point (a long-press context menu) can drive the same copy
flow. Each returned `TransferJob` owns its **own** `Sync` session rather than
borrowing the calling browser's, because a pull abort has to close the whole
session (RECV has no wire-level cancel) and that must not take down an
unrelated browse session. A job also watches its parent screen's/target
browser's `LV_EVENT_DELETE` (weakly) so a mid-transfer screen teardown ends the
transfer quietly instead of touching freed UI — the reason the
`lv_obj_add_event_fn` cleanup-ordering fix in
[gotchas.md](gotchas.md#lvgl-componentslvgl-app) was needed.

APK manifest parsing (`app/apk_info.{hpp,cpp}`) and image decoding
(`app/png_decode.{hpp,cpp}` + the `jpeg_decode_enhanced` Layer-1 whole-frame
decoder) both run **locally/offline where possible** — APK parsing needs no
device at all; image preview decodes off a low-priority Core-1 task and only
ever hands LVGL a small downscaled RGB565 frame, never the native-resolution
bitmap (mirrors the screenshot/screencap-preview decode split). Image preview
deliberately uses the bare JPEG decoder, not the PPA pipeline — see
[gotchas.md](gotchas.md#display--jpeg) (internal DMA RAM contention at
screen-stack depth).

## Parser contract (device_info, apk_info, media_session)

The device-facing parsers all follow one rule: **a field that fails to parse
is hidden/greyed, never surfaced as an error** — output formats vary too much
by Android vendor/version to treat a miss as exceptional (e.g. a SIM-less
device still reports a cellular signal level, so the cellular icon instead
gates on `gsm.sim.state`). Keep new parser fields consistent with this if you
add one.
