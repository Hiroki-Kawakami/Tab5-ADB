cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_SOURCE_DIR}/../esp-devkit/devkit.cmake)
devkit_idf_init(COMPONENT_DIRS ../app ../components)
project(tab5-adb)
