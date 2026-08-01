# Raw Opus decode seam. Keep target selection inside this middleware component
# so app code remains unaware of the device/simulator codec implementations.
if(ESP_PLATFORM)
    set(OPUS_DECODER_SRC "src/opus_decoder_esp.cpp")
else()
    set(OPUS_DECODER_SRC "src/opus_decoder_libopus.cpp")
endif()

# The simulator shim intentionally ignores REQUIRES; libopus is linked by the
# simulator root. On device this edge supplies Espressif's decoder API.
idf_component_register(
    SRCS ${OPUS_DECODER_SRC}
    INCLUDE_DIRS "inc"
    REQUIRES esp_audio_codec)
