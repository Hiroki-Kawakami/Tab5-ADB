// Device-only ADB smoke test (P6 bring-up). Temporary: exercises the usb_host
// transport end-to-end (connect + auth + a few shell commands) and logs results
// over the serial console. Call after adb_app() so the BSP has powered the USB
// host port. Remove once the LVGL UI (P7) drives the connection.
#pragma once

void adb_smoketest_start();
