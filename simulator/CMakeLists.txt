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

# ---------------------------------------------------------------------------
# Host dependencies
# ---------------------------------------------------------------------------
find_package(SDL2 REQUIRED)
find_package(CJSON REQUIRED)

# LVGL (lv_conf.h is picked up from this directory via LV_CONF_INCLUDE_SIMPLE)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        7d376cc5c4301f6dd1789a8bf3b1e56cdc44106a
)
FetchContent_MakeAvailable(lvgl)

# FreeRTOS-Kernel (POSIX port) so app/ code can use FreeRTOS primitives.
# Config header + the macOS/arm64 deadlock patch live under freertos/.
set(FREERTOS_PORT GCC_POSIX CACHE STRING "" FORCE)
set(FREERTOS_HEAP 3         CACHE STRING "" FORCE)
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/freertos)
FetchContent_Declare(
    freertos_kernel
    GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG        V11.1.0
    PATCH_COMMAND  python3 ${CMAKE_CURRENT_SOURCE_DIR}/freertos/patches/freertos_posix_macos.py
)
FetchContent_MakeAvailable(freertos_kernel)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(simulator)
target_compile_definitions(simulator PRIVATE
    SDL_DISABLE_ARM_NEON_H
    SDL_MAIN_HANDLED)
target_link_libraries(simulator SDL2::SDL2 lvgl freertos_kernel cjson ${CMAKE_DL_LIBS})

# Simulator-only sources: SDL/LVGL entry, host pf_port impl, IDF API shims.
# (idf_compat is on the include path so app/ code can use "esp_err.h",
# "esp_log.h" and the "freertos/*.h" ESP-IDF header paths on the host too.)
target_sources(simulator PRIVATE
    platform/main.cpp
    platform/platform_port_sim.cpp
    idf_compat/esp_err.c)
target_include_directories(simulator PRIVATE
    platform
    idf_compat)

# ---------------------------------------------------------------------------
# Shared components
# ---------------------------------------------------------------------------
# Every shared component (../components/*, ../app) describes itself with the
# ESP-IDF idf_component_register() call in its own CMakeLists.txt. On device
# that is the real IDF function; here we shim it to fold the component's
# sources / include dirs straight into the `simulator` executable.
#
# REQUIRES are intentionally ignored: everything links into a single binary so
# include dirs are global, and IDF-only requirements (esp_lvgl_port, driver,
# nvs_flash, ...) simply have no host counterpart.
function(idf_component_register)
    cmake_parse_arguments(C "" ""
        "SRCS;INCLUDE_DIRS;PRIV_INCLUDE_DIRS;REQUIRES;PRIV_REQUIRES;EMBED_FILES;EMBED_TXTFILES"
        ${ARGN})
    foreach(src ${C_SRCS})
        if(NOT IS_ABSOLUTE "${src}")
            set(src "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
        endif()
        target_sources(simulator PRIVATE "${src}")
    endforeach()
    foreach(dir ${C_INCLUDE_DIRS} ${C_PRIV_INCLUDE_DIRS})
        if(NOT IS_ABSOLUTE "${dir}")
            set(dir "${CMAKE_CURRENT_SOURCE_DIR}/${dir}")
        endif()
        target_include_directories(simulator PRIVATE "${dir}")
    endforeach()
endfunction()

# No-op stub for the other IDF component-CMake call shared components may use.
function(idf_component_set_property)
endfunction()

set(SHARED_COMPONENTS
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/platform_port
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/lvgl++
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/screen_manager
    ${CMAKE_CURRENT_SOURCE_DIR}/../app)
foreach(comp ${SHARED_COMPONENTS})
    get_filename_component(name "${comp}" NAME)
    add_subdirectory("${comp}" "${CMAKE_BINARY_DIR}/shared/${name}")
endforeach()
