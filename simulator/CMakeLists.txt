cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_SOURCE_DIR}/../esp-devkit/devkit.cmake)
devkit_simulator_init()
project(simulator C CXX)

devkit_simulator(
    BOARD tab5
    LV_CONF_DIR .
    MAIN_SRCS platform/main.cpp
    COMPONENT_DIRS
        ../components/embedded_adb
        ../components/adb
        ../components/agent_link
        ../components/wifi
        ../components/term_emu
        ../app)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MBEDTLS REQUIRED IMPORTED_TARGET mbedtls mbedcrypto mbedx509)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
target_link_libraries(simulator
    PkgConfig::MBEDTLS
    PkgConfig::LIBUSB
    PkgConfig::ZLIB)
