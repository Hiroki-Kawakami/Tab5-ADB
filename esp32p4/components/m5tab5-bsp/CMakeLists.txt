idf_component_register(
    SRCS
        "src/bsp_tab5.c"
        "src/audio_eq.c"
        "devices/pi4io/pi4io.c"
        "devices/ili9881c/ili9881c.c"
        "devices/gt911/gt911.c"
        "devices/st7123/st7123_lcd.c"
        "devices/st7123/st7123_touch.c"
        "devices/es8388/es8388.c"
    INCLUDE_DIRS "inc"
    PRIV_INCLUDE_DIRS "inc_private" "devices"
    PRIV_REQUIRES driver nvs_flash bt)

# Enable Link Time Optimization
target_compile_options(${COMPONENT_LIB} PRIVATE -flto)
target_link_libraries(${COMPONENT_LIB} PRIVATE -flto)
