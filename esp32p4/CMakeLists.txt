cmake_minimum_required(VERSION 3.16.0)
# Shared components: ../components is a container (each subdir is a component),
# ../app is itself a single component (it has its own CMakeLists.txt).
# Device-only components under ./components/ are auto-discovered by IDF.
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../components
    ${CMAKE_CURRENT_SOURCE_DIR}/../app)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5-adb)
