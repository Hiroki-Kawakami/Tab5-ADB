idf_component_register(
    SRCS "/dev/null"
    PRIV_REQUIRES adb agent_link bsp_rs devkit_core devkit_audio devkit_compression devkit_crypto devkit_gfx devkit_image devkit_net devkit_ui harness_rs idf media_engine rust_rt term_emu usb_host_rs)
devkit_rust_crate(ROOT "lib.rs" TESTS RESOURCES ../resources/resources.json)
