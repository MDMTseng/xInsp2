cmake_minimum_required(VERSION 3.16)
project(adder CXX C)
set(CMAKE_CXX_STANDARD 20)

# Point at the cloned xInsp2 (rebuild_plugins passes -DXINSP2_ROOT; the env var
# is the fallback for a manual `cmake` invocation).
if(NOT DEFINED XINSP2_ROOT AND DEFINED ENV{XINSP2_ROOT})
  set(XINSP2_ROOT $ENV{XINSP2_ROOT})
endif()
include(${XINSP2_ROOT}/sdk/cmake/xinsp2_plugin.cmake)

# --- external dependency -------------------------------------------------
# `mathx` lives outside the plugin (examples/calc_pipeline/vendor/mathx). Built
# here as a static lib and linked into the plugin. For a real vendor SDK you'd
# instead point target_include_directories / target_link_libraries at its
# headers + .lib (and ship its .dll next to adder.dll). See
# docs/guides/write-a-plugin.md (External libraries & CUDA).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/mathx
                 ${CMAKE_BINARY_DIR}/mathx)

# --- the plugin ----------------------------------------------------------
xinsp2_add_plugin(adder src/plugin.cpp)          # emits adder.dll next to this file
target_link_libraries(adder PRIVATE mathx)       # link the external dependency
