# xinsp2_plugin.cmake — shared CMake setup for xInsp2 plugins.
#
# A plugin's CMakeLists.txt only needs:
#
#   cmake_minimum_required(VERSION 3.16)
#   project(my_plugin CXX C)
#   set(CMAKE_CXX_STANDARD 20)
#
#   # Point at the cloned xInsp2 (env var or override on cmake command line)
#   if(NOT DEFINED XINSP2_ROOT AND DEFINED ENV{XINSP2_ROOT})
#       set(XINSP2_ROOT $ENV{XINSP2_ROOT})
#   endif()
#   include(${XINSP2_ROOT}/sdk/cmake/xinsp2_plugin.cmake)
#
#   xinsp2_add_plugin(my_plugin my_plugin.cpp)
#
# This module never touches anything inside the xInsp2 tree — it only
# reads headers + cJSON.c from there. The plugin DLL is dropped right
# next to its plugin.json so the host can pick it up via --plugins-dir.

if(NOT DEFINED XINSP2_ROOT OR NOT EXISTS "${XINSP2_ROOT}/backend/include/xi/xi_abi.hpp")
    message(FATAL_ERROR
        "XINSP2_ROOT is not set or does not point at a cloned xInsp2 repo.\n"
        "Set the XINSP2_ROOT env var or pass -DXINSP2_ROOT=<path> on the cmake command line.\n"
        "Currently: XINSP2_ROOT='${XINSP2_ROOT}'")
endif()

set(XINSP2_INCLUDE ${XINSP2_ROOT}/backend/include)
set(XINSP2_VENDOR  ${XINSP2_ROOT}/backend/vendor)
set(XINSP2_CJSON   ${XINSP2_VENDOR}/cJSON.c)

if(MSVC)
    add_compile_options(/W3 /utf-8)
endif()

# OpenCV: mandatory. xInsp2 headers (xi.hpp / xi_plugin_support.hpp)
# pull in <opencv2/opencv.hpp> for image operators, so every plugin
# needs OpenCV's include + libs visible.
if(NOT DEFINED OpenCV_DIR AND NOT DEFINED ENV{OpenCV_DIR})
    foreach(_d "C:/opencv/opencv/build/x64/vc16/lib"
               "C:/opencv/opencv/build/x64/vc17/lib"
               "C:/opencv/build/x64/vc16/lib"
               "C:/opencv/build/x64/vc17/lib")
        if(EXISTS "${_d}/OpenCVConfig.cmake")
            set(OpenCV_DIR "${_d}")
            break()
        endif()
    endforeach()
endif()
find_package(OpenCV REQUIRED COMPONENTS core imgcodecs imgproc)

# xinsp2_add_plugin(<target> <sources...>)
#
# Builds a plugin SHARED library with cJSON linked in. The output DLL is
# placed next to the plugin's plugin.json so --plugins-dir can find it.
function(xinsp2_add_plugin name)
    add_library(${name} SHARED ${ARGN} ${XINSP2_CJSON})
    target_include_directories(${name} PRIVATE
        ${XINSP2_INCLUDE}
        ${XINSP2_VENDOR})
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(${name} PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(${name} PRIVATE ${OpenCV_LIBS})
endfunction()

# xinsp2_add_plugin_test(<target> <sources...>)
#
# Builds a developer-side test executable that links the C ABI surface
# (no SHARED — the test loads the plugin DLL via LoadLibrary).
function(xinsp2_add_plugin_test target_name)
    add_executable(${target_name} ${ARGN} ${XINSP2_CJSON})
    target_include_directories(${target_name} PRIVATE
        ${XINSP2_INCLUDE}
        ${XINSP2_VENDOR})
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()
