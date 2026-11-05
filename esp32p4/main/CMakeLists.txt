idf_component_register(
    SRCS "/dev/null"
    PRIV_REQUIRES app rust_rt)
devkit_rust_crate(ROOT "main.rs")

# Keep app_main() (referenced across the main<->app component link group) from
# being garbage-collected.
idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE ON)
