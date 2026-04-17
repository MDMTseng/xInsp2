cmake_minimum_required(VERSION 3.16)
project(hello CXX C)
set(CMAKE_CXX_STANDARD 20)
if(MSVC)
    add_compile_options(/W3 /utf-8)
endif()

if(NOT DEFINED XINSP2_ROOT)
    set(XINSP2_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
endif()

add_library(hello SHARED hello.cpp ${XINSP2_ROOT}/backend/vendor/cJSON.c)
target_include_directories(hello PRIVATE
    ${XINSP2_ROOT}/backend/include
    ${XINSP2_ROOT}/backend/vendor)
set_target_properties(hello PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR})
