cmake_minimum_required(VERSION 3.16.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../esp-devkit/devkit.cmake)
devkit_idf_init(UI_FRAMEWORK COMPONENT_DIRS ../components ../app)
project(tab5-adb)
