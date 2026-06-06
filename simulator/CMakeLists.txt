cmake_minimum_required(VERSION 3.16)
include(FetchContent)

set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_COMPILER_LAUNCHER ccache)
set(CMAKE_CXX_COMPILER_LAUNCHER ccache)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

project(simulator C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Load Dependencies
find_package(SDL2 REQUIRED)
find_package(CJSON REQUIRED)

# Load LVGL (lv_conf.h is picked up from this directory via LV_CONF_INCLUDE_SIMPLE)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        7d376cc5c4301f6dd1789a8bf3b1e56cdc44106a
)
FetchContent_MakeAvailable(lvgl)

# Load FreeRTOS-Kernel (POSIX port) so app/ code can use FreeRTOS primitives
set(FREERTOS_PORT GCC_POSIX CACHE STRING "" FORCE)
set(FREERTOS_HEAP 3         CACHE STRING "" FORCE)
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/freertos_config)
FetchContent_Declare(
    freertos_kernel
    GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG        V11.1.0
    # Fix a macOS/arm64 deadlock in the POSIX port's task pthread creation.
    PATCH_COMMAND  python3 ${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos_posix_macos.py
)
FetchContent_MakeAvailable(freertos_kernel)

# Import components/* and an app directory into a target.
# Usage: target_add_components(TARGET APP_DIR)
function(target_add_components TARGET APP_DIR)
    file(GLOB COMPONENTS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/../components/*")
    list(APPEND COMPONENTS ${APP_DIR})
    message(STATUS "COMPONENTS=${COMPONENTS}")

    foreach(COMPONENT ${COMPONENTS})
        if(IS_DIRECTORY ${COMPONENT})
            file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS ${COMPONENT}/*.c ${COMPONENT}/*.cpp)
            target_sources(${TARGET} PRIVATE ${SOURCES})
            target_include_directories(${TARGET} PRIVATE ${COMPONENT})
        endif()
    endforeach()
endfunction()

add_executable(simulator)
file(GLOB_RECURSE DEVICE_SOURCES CONFIGURE_DEPENDS
    src/*.c src/*.cpp
    tab5-bsp_simulator/*.c tab5-bsp_simulator/*.cpp)
target_compile_definitions(simulator PRIVATE
    SDL_DISABLE_ARM_NEON_H
    SDL_MAIN_HANDLED)
target_sources(simulator PRIVATE ${DEVICE_SOURCES})
target_include_directories(simulator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tab5-bsp_simulator
    ${CMAKE_CURRENT_SOURCE_DIR}/tab5-bsp_simulator/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../idf-components/main
    ${CMAKE_CURRENT_SOURCE_DIR}/freertos_shim)
target_link_libraries(simulator SDL2::SDL2 lvgl freertos_kernel cjson ${CMAKE_DL_LIBS})
target_add_components(simulator ${CMAKE_CURRENT_SOURCE_DIR}/../app)
