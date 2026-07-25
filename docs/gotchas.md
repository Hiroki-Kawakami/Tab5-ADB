# Gotchas

Non-obvious pitfalls hit during development — mostly hardware/vendor-stack
quirks that cost real debugging time and would otherwise get rediscovered.
Grouped by area; each entry names the file that carries the fix so you can jump
to the code and its comments.

## USB host (`components/embedded_adb/src/transport_usbhost.cpp`)

- **FIFO bias caps bulk OUT at 256 bytes.** The default `usb_host_config_t` FIFO
  bias (`BALANCED`) gives the non-periodic TX FIFO only `dfifo_depth/16` lines,
  which caps bulk OUT MPS at 256 — too small to claim an Android phone's 512-byte
  high-speed bulk endpoints (`interface_claim → ESP_ERR_NOT_SUPPORTED`). Since
  ADB's hot direction is bulk IN (the mirror), the fix biases
  `fifo_settings_custom` toward RX: **rx=512 / nptx=192 / ptx=0** lines.
- **High-throughput bulk IN intermittently corrupts a payload byte — it's RX-FIFO
  starvation, not a large-transfer quirk.** At the mirror's ~2-4 MB/s throughput
  the P4 usb_host would occasionally corrupt one byte of a bulk-IN payload,
  producing a malformed JPEG strip. The first hypothesis ("DWC2 large-transfer
  quirk") was wrong and its mitigation (reading each payload in ≤4 KB chunks)
  only masked the bug at low rate. The real cause: the FIFO above overflows when
  the USB DMA drain stalls under concurrent PSRAM traffic (panel refresh + JPEG
  decode). Fix: the RX-biased FIFO **plus** reading each `A_WRTE` payload in
  **one** transfer, not chunks (adbd writes it with a single `usb_write`, so it
  already arrives as one bulk-IN stream). A multi-in-flight transfer pool
  (UVC-style read-ahead) was also tried to keep the endpoint busy during decode,
  but hung the reader after ~2s — ADB's lossless backpressure needs
  cross-thread re-submission that UVC's drop-on-overflow callback sidesteps.
  Not pursued; the single-transfer reader is already clean at 40-49 fps.
- **A device plugged in before "Connect" never enumerates.** The host stack
  installs lazily (on Connect), but VBUS (PI4IO `USB5V_EN`) may already be on, so
  a pre-plugged phone is already attached when the root port powers and the DWC2
  never sees an idle→connected edge. Toggling the controller's internal
  root-port-power does **not** fix it (the phone's physical VBUS never cycles).
  Fix: after `usb_host_install`, power-cycle VBUS itself (off → 200ms → on) via
  an app-supplied hook (`adb::set_usb_host_reset_hook`) — a real re-attach edge.
  `embedded_adb` only knows "reset the port"; the VBUS policy lives in
  `adb_app` (see [adb.md](adb.md)).
- **Teardown must fully uninstall the usb_host stack or reconnect fails with
  `ESP_ERR_INVALID_STATE`.** `usb_host_install` refuses if the previous instance
  didn't uninstall cleanly. The two background event tasks
  (`usb_host_lib_handle_events` / `usb_host_client_handle_events`) must be
  deterministically joined before `usb_host_uninstall`, and the uninstall call
  itself needs to keep pumping `usb_host_lib_handle_events` (no task left to do
  it) until it succeeds — a bare `vTaskDelay` + ignored return silently leaves
  the stack installed. Device-only; the sim's libusb transport can't reproduce
  it and it still needs a real-HW flash check.

## Wi-Fi / esp-hosted (`components/wifi/`, `esp32p4/sdkconfig.defaults`)

- **Host `esp_hosted` version must exactly match the ESP32-C6 slave firmware.**
  A 1.x-host/2.x-slave mismatch runs, but very slowly. Pinned in
  `components/wifi/idf_component.yml`. Reset GPIO polarity also flipped
  between major versions (1.x = active-low, 2.x = active-high) — wrong polarity
  holds the C6 in reset and the SDIO bring-up loops on `send_op_cond`.
- **C6 firmware fires redundant `WIFI_EVENT_STA_START`/`STOP` events**, which
  crashes IDF's default `wifi_default_action_sta_start` handler (registered by
  `esp_netif_create_default_wifi_sta()`) on the second invocation. Fix:
  `backend_espwifi.cpp` creates the STA netif manually and registers its own
  **idempotent** start/stop handlers instead of the IDF default. Device-only,
  not reproducible in the sim fake.
- **SD card and the C6 Wi-Fi link share one SDMMC host with two slots — they
  must not collide.** The C6 SDIO link must stay on slot 1 (`esp-hosted`
  default) and the SD card on slot 0; `SDMMC_HOST_DEFAULT()` picks slot 1, so the
  SD driver must override it explicitly. Putting both on the same slot resets
  the live C6 SDIO bus mid-transfer (`sdmmc_io_rw_extended ... 0x107`). This
  flipped once already: esp-hosted 1.4.0 ran the C6 on slot 0 (SD's default slot
  1 was free); esp-hosted 2.x moved the C6 to slot 1, silently breaking the SD
  until it too moved to slot 0. IDF 6 permits only one initialization of that
  host controller, so the Tab5 SD provider borrows the esp-hosted-owned host but
  still initializes and deinitializes its own slot 0. Making slot deinit a no-op
  breaks retry after a failed mount or card reinsertion.
- **A `>65535` lwip TCP window silently gets rejected back to 5760** unless
  `LWIP_WND_SCALE` is enabled, which itself `depends on
  SPIRAM_TRY_ALLOCATE_WIFI_LWIP`. The mirror-over-Wi-Fi tuning in
  `sdkconfig.defaults` sidesteps this by using the max non-scaled window
  (65534) instead of scaling.
- **`LWIP_IRAM_OPTIMIZATION` looks like a throughput win but breaks boot.** It
  moves ~15KB of LWIP code from flash to internal SRAM; esp-hosted 2.x already
  eats most internal RAM at init, so enabling it fails the timer-task stack
  allocation at boot (`vApplicationGetTimerTaskMemory ...
  pxStackBufferTemp != NULL`). Deliberately left off.
- **ESP-IDF's mbedTLS has TLS 1.3 off by default**, needed for Android 11+
  wireless-debugging pairing. `esp32p4/sdkconfig.defaults` sets
  `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y` (+~64KB binary). IDF 6 also disables X.509
  creation by default, but STARTTLS creates its client certificate at runtime;
  `CONFIG_MBEDTLS_X509_CREATE_C` and `CONFIG_MBEDTLS_X509_CRT_WRITE_C` must stay
  enabled. Six-digit pairing also needs
  `CONFIG_MBEDTLS_SSL_KEYING_MATERIAL_EXPORT`; without it TLS succeeds but the
  SPAKE2 password cannot match Android. Both targets use Mbed TLS 4's PSA Crypto
  API, and the host flake pins `mbedtls_4` so the simulator cannot silently keep
  compiling the removed 3.x APIs. Mbed TLS 4 reports a successfully processed
  TLS 1.3 post-handshake `NewSessionTicket` from `mbedtls_ssl_read()` as
  `MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET`; it must be retried rather than
  treated as a fatal read error, or ADB closes before receiving the encrypted
  `CNXN`.

## Touch (`esp-devkit/bsp/boards/tab5/tab5.c`)

- **GT911 touch is dead after a warm reboot** (`esp_restart`/panic, i.e.
  `SW_CPU_RESET` — a cold power-on is fine). Fix is the AOSP `gtp_reset_guitar`
  sequence (INT held low ~100ms during the boot reset step), not a power or
  peripheral reset. Reference: the local `gt911_hotknot` clone (see
  `[[local-reference-clones]]` in memory — not in this repo).

## Display / JPEG

- **RGB565 output needs `rgb_order = BGR`, not `RGB`**
  (`esp-devkit/libs/jpeg_decode_enhanced/`). The Tab5 panel is driven R-in-high-bits
  RGB565; on the P4 the *BGR* 2D-DMA scramble is what produces that packing —
  the *RGB* enum's scramble mis-packs the 16-bit pixel (green straddles the byte
  boundary) and renders as rainbow noise with the image structure intact. The
  simulator's libjpeg shim mirrors this so sim previews match device colours.
- **The P4 JPEG 2D-DMA cannot stride** — it can only write a tightly-packed
  `w`×`h` picture, never a narrower picture into a wider destination buffer with
  a row pitch. Two hardware attempts to add stride support both mis-rendered
  (a black band per strip). This is why the mirror agent always emits
  full-panel-width strips (see [agent.md](agent.md)) — a tight decode into
  `fb + y*pitch` is then a placed decode for free.
- **The PPA pipeline and the bare JPEG decoder compete for internal DMA RAM.**
  `jpeg_ppa_pipeline_new` registers its PPA client (grabbing internal DMA)
  *before* creating the JPEG engine, so at screen-stack depth (where
  esp-hosted/Wi-Fi already hold most internal RAM) the engine alloc fails.
  Throughput-oriented screens using these drivers must account for their stack
  depth. File previews and PNG screenshot paths use the CPU `image_framework`
  decoder; `ScreencapPreview` retains PPA only for its optional JPEG path.

## Android agent / app_process (`android-agent/src/`)

`app_process` runs with none of the Zygote-preloaded app scaffolding, so
ordinary app-facing Android APIs fail in surprising ways:

- **No default `Typeface`** (`Typeface.DEFAULT`'s native instance is 0), so any
  legacy text-drawing path aborts/throws. Fix (`TextRender.java`): build a
  self-contained Typeface via the low-level `Font`/`FontFamily` +
  `Typeface.nativeCreateFromArray(fallback=0)` reflection over Roboto +
  `/system/fonts/NotoSansCJK*`, and set it explicitly on every `Paint`.
- **`getSystemService("media_session")` NPEs** (the media framework initializer
  never ran). Fix (`MediaInfo.java`): reach the session manager over the
  **binder** directly — `ServiceManager.getService("media_session")` →
  `ISessionManager$Stub.asInterface` → `getSessions(null, 0, 0)` (shell uid
  holds `MEDIA_CONTENT_CONTROL`).
- **Split-APK launcher icons don't resolve.** For split-APK installs the icon
  bitmap lives in `split_config.<dpi>.apk`, and `getApplicationIcon` under a
  synthetic context falls back to the (also-broken) framework default. Fix
  (`AppInfo.java`): build a `Resources` over base + all splits via the hidden
  `ApkAssets.loadFromPath` + `AssetManager.setApkAssets` (merges same-package-id
  tables across splits — the legacy `addAssetPath` does not, so density never
  resolves), plus `SystemContext`'s `fillAppContext` (a bare `Application`
  wrapping the system context) to avoid an NPE in
  `ActivityThread.currentApplication()` while inflating the icon XML.
- **Now-playing album art frequently arrives after the track metadata** (the app
  pushes text before its art bitmap finishes loading), so a `content_token`
  change often first renders art-less. The Tab5 must re-fetch
  `GET_MEDIA_RENDER` when a later `MEDIA` event flips `has_art` 0→1 for the
  *same* token, not just on token change (`ADBDeviceScreen`'s
  `maybeFetchRender`).

See also `[[agent-media-phase0]]` in memory for the full app_process media
investigation.

## LVGL

- **`lv_async_call` callbacks run in LIFO order**, not FIFO. Anything that needs
  ordered delivery of a stream of chunks (shell/logcat output, sync completions)
  must buffer and coalesce into one async call per flush, not queue one async
  call per chunk. See `[[lv-async-call-lifo]]` in memory.
- **`lv_obj_add_event_fn`'s cleanup ordering matters for `LV_EVENT_DELETE`
  handlers.** It registers a cleanup callback that frees the heap
  `std::function` on delete; callbacks fire in registration order, so a handler
  *filtered on* `LV_EVENT_DELETE` must be registered **before** its own cleanup
  or it runs after its function is already freed (`bad_function_call`). This is
  what the file-transfer job's parent-teardown watcher relies on.
- **Deleting an active screen inside its own event handler segfaults.**
  `ScreenManager`'s pop/load must call `lv_screen_load(next)` *before* freeing
  the leaving screen, and defer the leaving screen's destruction via
  `lv_async_call` — never delete the screen that's still on the event-dispatch
  stack.
- **`lv_keyboard` pre-sets `ALIGN_BOTTOM_MID`**, so a plain `lv_obj_set_pos`
  offsets from the bottom, not the top — re-`align` it instead. It also drops
  `CLICK_FOCUSABLE`, so re-tapping an already-focused textarea sends no
  `FOCUSED` event; hook `LV_EVENT_CLICKED` too if you need to react to that tap
  (the logcat filter box and the Wi-Fi password modal both need this).
- **Deleting an `lv_keyboard` inside its own in-flight button event hangs
  LVGL** — defer the delete via `lv_async_call` (plain `modal_confirm` dialogs
  don't hit this, only keyboards).
