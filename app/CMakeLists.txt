idf_component_register(
    SRCS "/dev/null"
    PRIV_REQUIRES bsp_rs devkit devkit_gfx devkit_ui idf rust_rt)
devkit_rust_crate(ROOT "lib.rs" TESTS RESOURCES ../resources/resources.json)
