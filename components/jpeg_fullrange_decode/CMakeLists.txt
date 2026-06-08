# Shared component: full-range BT.601 JPEG decode. Same target-divergent shape as
# m5stack-bsp / embedded_adb's transport — one public header, ESP_PLATFORM picks
# the implementation:
#   device  -> src/jpeg_fullrange_decode_p4.c : the real P4 driver, overriding the
#              2D-DMA CSC matrix registers for full-range (JFIF) output. Reaches
#              into IDF-internal esp_driver_jpeg headers (see below).
#   host    -> src/jpeg_fullrange_decode_sim.c : delegates to the idf_compat
#              libjpeg shim (already full-range), so app code calls the same
#              jpeg_decoder_process_full_range() on both targets.
if(ESP_PLATFORM)
    set(jfr_srcs "src/jpeg_fullrange_decode_p4.c")
    set(jfr_priv_reqs esp_driver_jpeg hal esp_mm esp_hw_support esp_pm driver)
else()
    set(jfr_srcs "src/jpeg_fullrange_decode_sim.c")
    set(jfr_priv_reqs "")
endif()

idf_component_register(
    SRCS ${jfr_srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${jfr_priv_reqs}
)

# Device only: the P4 impl reuses the IDF jpeg engine but reimplements the
# per-frame process step, so it needs IDF-internal headers (jpeg_private.h,
# private/*.h) that esp_driver_jpeg does not export. ${COMPONENT_LIB} exists only
# in the device (real IDF) build; the simulator shim ignores REQUIRES and folds
# sources into the exe, so this block is skipped there.
if(ESP_PLATFORM)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "$ENV{IDF_PATH}/components/esp_driver_jpeg"
        "$ENV{IDF_PATH}/components/esp_driver_jpeg/private")
endif()
