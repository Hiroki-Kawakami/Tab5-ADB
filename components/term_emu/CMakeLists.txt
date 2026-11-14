# A VT100/xterm-subset terminal emulator: output bytes in, a cell grid out.
idf_component_register(
    SRCS "/dev/null"
    PRIV_REQUIRES rust_rt unicode)
devkit_rust_crate(ROOT "src/lib.rs" TESTS)
