# The Tab5 end of the link to tab5adb-agent, over adb streams.
idf_component_register(
    SRCS "/dev/null"
    PRIV_REQUIRES adb devkit idf rust_rt)
devkit_rust_crate(ROOT "src/lib.rs" TESTS)
