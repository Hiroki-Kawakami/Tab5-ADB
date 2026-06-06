file(GLOB_RECURSE MAIN_SRCS "*.c" "*.cpp")
set(MAIN_INCLUDE_DIRS ".")

# Import Common & App Components
file(GLOB COMPONENTS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/../../components/*")
list(APPEND COMPONENTS ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

foreach(COMPONENT ${COMPONENTS})
    if(IS_DIRECTORY ${COMPONENT})
        file(GLOB_RECURSE SOURCES ${COMPONENT}/*.c ${COMPONENT}/*.cpp)
        list(APPEND MAIN_SRCS ${SOURCES})
        list(APPEND MAIN_INCLUDE_DIRS ${COMPONENT})
    endif()
endforeach()

message(STATUS  "COMPONENTS=${COMPONENTS}")
message(STATUS  "MAIN_SRCS=${MAIN_SRCS}")
message(STATUS  "MAIN_INCLUDE_DIRS=${MAIN_INCLUDE_DIRS}")

idf_component_register(SRCS ${MAIN_SRCS}
                       INCLUDE_DIRS ${MAIN_INCLUDE_DIRS})
idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE ON)
