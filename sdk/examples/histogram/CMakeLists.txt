cmake_minimum_required(VERSION 3.16)
project(histogram CXX C)
set(CMAKE_CXX_STANDARD 20)

if(NOT DEFINED XINSP2_ROOT)
    set(XINSP2_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
endif()
include(${XINSP2_ROOT}/sdk/cmake/xinsp2_plugin.cmake)

xinsp2_add_plugin(histogram histogram.cpp)
