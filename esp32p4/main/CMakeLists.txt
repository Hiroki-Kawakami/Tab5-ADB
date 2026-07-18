idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES app)

# Keep app_main() (referenced across the main<->app component link group) from
# being garbage-collected.
idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE ON)
