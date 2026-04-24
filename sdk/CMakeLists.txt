cmake_minimum_required(VERSION 3.16)
project(xinsp2_sdk_examples CXX C)

# Builds all SDK examples at once. Invoke from anywhere with:
#   cmake -S sdk -B sdk/build -A x64
#   cmake --build sdk/build --config Release

if(NOT DEFINED XINSP2_ROOT)
    set(XINSP2_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
endif()

add_subdirectory(examples/hello)
add_subdirectory(examples/counter)
add_subdirectory(examples/invert)
add_subdirectory(examples/histogram)
add_subdirectory(examples/trigger_source)
