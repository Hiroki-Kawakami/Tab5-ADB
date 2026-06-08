/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Sim harness — scripted, headless UI verification driver for the simulator.
 *
 * Instead of an interactive SDL window driven by a real mouse (and verified by
 * osascript screenshots of the host display), the harness runs the exact same
 * app/BSP code under SIMULATOR_HEADLESS and drives it from a text script: it
 * pumps the LVGL loop, injects synthetic touch, and captures the framebuffer to
 * an image file. Everything runs on the main (LVGL) thread, so capture never
 * races rendering, and the run is deterministic and host-display-free.
 *
 * Selected by the SIMULATOR_SCRIPT env var in the simulator entry (main.cpp);
 * SIMULATOR_HEADLESS is honoured by the SDL backend independently.
 *
 * Script commands (one per line; '#' and blank lines ignored):
 *   wait <ms>            pump the LVGL loop for <ms>
 *   settle [<max_ms>]    pump until no LVGL timer/anim is pending (default 5000)
 *   capture <path.jpg>   write the latest frame as a JPEG
 *   tap <x> <y>          synthetic press+release at panel coords (one click)
 *   down <x> <y>         press/hold (no release) — start of a drag
 *   move <x> <y>         move the held pointer (drag/swipe)
 *   up                   release the held pointer
 *   quit                 stop the run (implicit at end of script)
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Run the script at script_path ("-" = stdin), then return a process exit code
 * (0 on success). Call after adb_app(); it owns the main thread until quit. */
int sim_harness_run(const char *script_path);

#ifdef __cplusplus
}
#endif
