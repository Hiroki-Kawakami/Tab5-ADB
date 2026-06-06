file(GLOB_RECURSE BSP_SRCS "*.c")
idf_component_register(
    SRCS ${BSP_SRCS}
    INCLUDE_DIRS "inc"
    PRIV_INCLUDE_DIRS "inc_private" "devices"
    PRIV_REQUIRES driver nvs_flash bt)

# Enable Link Time Optimization
target_compile_options(${COMPONENT_LIB} PRIVATE -flto)
target_link_libraries(${COMPONENT_LIB} PRIVATE -flto)
