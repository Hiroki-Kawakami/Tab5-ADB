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
find_package(Threads REQUIRED)   # FreeRTOS POSIX port (vendored in idf_compat) uses pthreads

# LVGL (lv_conf.h is picked up from this directory via LV_CONF_INCLUDE_SIMPLE)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        7d376cc5c4301f6dd1789a8bf3b1e56cdc44106a
)
FetchContent_MakeAvailable(lvgl)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(simulator)
target_compile_definitions(simulator PRIVATE
    SDL_DISABLE_ARM_NEON_H
    SDL_MAIN_HANDLED)
target_link_libraries(simulator SDL2::SDL2 lvgl cjson Threads::Threads ${CMAKE_DL_LIBS})

# The executable's own entry: the SDL/LVGL + FreeRTOS main loop. This is the
# simulator's "main" (analogous to esp32p4/main on device) — it owns the host
# LVGL runtime, while the hardware seam (display/touch) lives in the m5stack-bsp
# simulator backend below. It stays a direct source rather than a component.
target_sources(simulator PRIVATE
    platform/main.cpp)
target_include_directories(simulator PRIVATE platform)

# ---------------------------------------------------------------------------
# Components (idf_component_register shim)
# ---------------------------------------------------------------------------
# Both the simulator-only layer (idf_compat) and the shared components
# (../components/*, ../app) describe themselves with the ESP-IDF
# idf_component_register() call in their own CMakeLists.txt. On device that is
# the real IDF function; here we shim it to fold the component's sources /
# include dirs straight into the `simulator` executable.
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

# Simulator-only components (host shims for ESP-IDF APIs).
add_subdirectory(idf_compat "${CMAKE_BINARY_DIR}/sim/idf_compat")

# Shared components (built for both targets). m5stack-bsp's CMakeLists.txt picks
# its simulator backend (SDL) here since ESP_PLATFORM is unset.
set(SHARED_COMPONENTS
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/m5stack-bsp
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/lvgl++
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/screen_manager
    ${CMAKE_CURRENT_SOURCE_DIR}/../app)
foreach(comp ${SHARED_COMPONENTS})
    get_filename_component(name "${comp}" NAME)
    add_subdirectory("${comp}" "${CMAKE_BINARY_DIR}/shared/${name}")
endforeach()
