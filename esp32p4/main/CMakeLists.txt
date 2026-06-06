# Device entry point + on-device pf_port implementation (BSP + esp_lvgl_port).
# `main` is the IDF special component: it implicitly depends on every other
# component in the build, so no REQUIRES is needed (and adding one would
# suppress that implicit dependency, breaking transitive BSP/LCD includes).
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS ".")

# Keep app_main() and the pf_port symbols (referenced across the main<->app
# component link group) from being garbage-collected.
idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE ON)
