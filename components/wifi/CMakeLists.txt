# wifi — app-facing Wi-Fi STA connection manager (see README.md).
#
# Portable Manager (state machine, NVS persistence, listener dispatch, timeout)
# plus ONE device/simulator backend split — the same shape as embedded_adb's
# transport: the esp_wifi API is too large to reimplement on the host, so the
# backend is ESP_PLATFORM-branched rather than an idf_compat esp_wifi shim.
#   device:    backend_espwifi.cpp (esp_wifi / esp_netif / esp_event; on the P4
#              this is esp_wifi_remote over the C6 esp-hosted SDIO link)
#   simulator: backend_sim.cpp (deterministic scriptable fake, wifi_sim.hpp)
set(WIFI_COMMON_SRCS "src/wifi_manager.cpp")

if(ESP_PLATFORM)
    # esp_wifi.h comes from the esp_wifi component (its impl is provided by
    # esp_wifi_remote over the C6 esp-hosted link on the P4); esp_wifi_remote
    # PRIV_REQUIRES esp_wifi, so it isn't propagated — REQUIRE esp_wifi here.
    idf_component_register(
        SRCS ${WIFI_COMMON_SRCS} "src/backend_espwifi.cpp"
        INCLUDE_DIRS "inc"
        PRIV_INCLUDE_DIRS "src"
        PRIV_REQUIRES esp_wifi esp_wifi_remote esp_netif esp_event nvs_flash freertos)
else()
    # Simulator: esp_wifi/esp_netif/esp_event have no host counterpart; the fake
    # backend needs none. NVS + FreeRTOS come from idf_compat.
    idf_component_register(
        SRCS ${WIFI_COMMON_SRCS} "src/backend_sim.cpp"
        INCLUDE_DIRS "inc"
        PRIV_INCLUDE_DIRS "src")
endif()
