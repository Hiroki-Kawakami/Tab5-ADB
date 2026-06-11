# term_emu — VT100/xterm-subset terminal emulator (cell grid + escape parser).
# Pure portable C++ with no LVGL/adb dependency: PTY bytes in, Cell grid out.
# The ADB shell terminal screen renders the grid; host unit tests in test/.
# Grid/scrollback storage is PSRAM (heap_caps_malloc) — `heap` is an IDF common
# component, so no REQUIRES is needed.
idf_component_register(
    SRCS "src/vt_parser.cpp" "src/term_emu.cpp"
    INCLUDE_DIRS "inc")
