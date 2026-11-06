# ADB host: connection, streams and the shell/sync services, over USB.
idf_component_register(
    SRCS "/dev/null"
    PRIV_REQUIRES devkit devkit_crypto idf rust_rt usb_host_rs)
devkit_rust_crate(ROOT "src/lib.rs" TESTS)
