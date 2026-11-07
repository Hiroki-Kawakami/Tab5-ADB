idf_component_register(
    SRCS "/dev/null"
    PRIV_REQUIRES adb agent_link bsp_rs devkit devkit_crypto devkit_gfx devkit_net devkit_ui harness_rs idf rust_rt usb_host_rs)
devkit_rust_crate(ROOT "lib.rs" TESTS RESOURCES ../resources/resources.json)
