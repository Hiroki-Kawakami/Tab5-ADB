# Device: the real strip-pipelined HW JPEG + PPA implementation (depends on the
# P4 JPEG/2D-DMA HW + ESP-IDF private headers). Simulator: host implementations
# of the Layer 1 whole-frame path (jpeg_enh_decoder_process) and the Layer 2
# (jpeg_ppa_pipeline) API, backed by the idf_compat libjpeg decode shim + the
# software PPA shim — same per-target branch as m5stack-bsp. (The libjpeg shim
# already decodes JFIF full-range, so the sim reproduces the device's
# full-range colours.)
if(ESP_PLATFORM)
    idf_component_register(
        SRCS
            "src/jpeg_decode_enhanced.c"
            "src/jpeg_ppa_pipeline.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_driver_jpeg esp_driver_ppa
        PRIV_REQUIRES hal esp_mm esp_hw_support driver
    )

    # NOTE: depends on ESP-IDF internal headers (jpeg_private.h, private/*.h,
    # esp_private/dma2d.h, hal/jpeg_ll.h). These live inside the esp_driver_jpeg
    # and esp_hw_support components and are not exported as public include dirs,
    # so we add them explicitly. jpeg_decode_enhanced.c carries an ESP_IDF_VERSION
    # guard because the private struct layouts can change between IDF releases.
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "$ENV{IDF_PATH}/components/esp_driver_jpeg"
        "$ENV{IDF_PATH}/components/esp_driver_jpeg/private"
    )
else()
    idf_component_register(
        SRCS
            "src/jpeg_decode_enhanced_sim.c"
            "src/jpeg_ppa_pipeline_sim.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_driver_jpeg esp_driver_ppa
    )
endif()
