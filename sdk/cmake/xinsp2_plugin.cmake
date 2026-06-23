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
# reads headers + yyjson.c from there. The plugin DLL is dropped right
# next to its plugin.json so the host can pick it up via --plugins-dir.

if(NOT DEFINED XINSP2_ROOT OR NOT EXISTS "${XINSP2_ROOT}/backend/include/xi/xi_abi.hpp")
    message(FATAL_ERROR
        "XINSP2_ROOT is not set or does not point at a cloned xInsp2 repo.\n"
        "Set the XINSP2_ROOT env var or pass -DXINSP2_ROOT=<path> on the cmake command line.\n"
        "Currently: XINSP2_ROOT='${XINSP2_ROOT}'")
endif()

set(XINSP2_INCLUDE ${XINSP2_ROOT}/backend/include)
set(XINSP2_VENDOR  ${XINSP2_ROOT}/backend/vendor)
set(XINSP2_YYJSON  ${XINSP2_VENDOR}/yyjson/yyjson.c)

if(MSVC)
    # /EHa (async exceptions) is REQUIRED, not cosmetic: the host runs every
    # plugin call under _set_se_translator (xi_seh.hpp), turning an SEH fault
    # into a C++ exception it absorbs. MSVC only emits the unwind funclets that
    # make that translation sound — and that run local dtors during unwind —
    # under /EHa. Under the default /EHsc the host catch still fires but RAII in
    # the faulting plugin frame is skipped, leaking its pool_image handles
    # (1 ImagePool slot per absorbed crash). Matches the backend EXE and the
    # in-tree compiler (xi_script_compiler.hpp) + docs/architecture.md.
    add_compile_options(/W3 /utf-8 /EHa)
endif()

# OpenCV: mandatory. xInsp2 headers (xi.hpp / xi_plugin_support.hpp) pull in
# <opencv2/opencv.hpp>, so every plugin (and any SDK-adjacent test/aux exe) needs
# OpenCV visible. Plain `find_package(OpenCV)` with OpenCV_DIR pointing at the
# top-level pack dir reports OpenCV_FOUND=FALSE (that config is a stub) — the
# resolvable level is `.../build/x64/vcNN/lib`. This macro probes those candidates
# and find_package's OpenCV; it's exposed so an auxiliary CMake (a verify exe, a
# tool) can reuse the SDK's OpenCV-locating logic instead of re-implementing the
# foreach:  include(xinsp2_plugin.cmake); xinsp2_find_opencv();
macro(xinsp2_find_opencv)
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
endmacro()
xinsp2_find_opencv()   # run once at include so xinsp2_add_plugin* have OpenCV

# xinsp2_add_plugin(<target> <sources...>)
#
# Builds a plugin SHARED library with yyjson linked in. The output DLL is
# placed next to the plugin's plugin.json so --plugins-dir can find it.
function(xinsp2_add_plugin name)
    add_library(${name} SHARED ${ARGN} ${XINSP2_YYJSON})
    target_include_directories(${name} PRIVATE
        ${XINSP2_INCLUDE}
        ${XINSP2_VENDOR}
        ${XINSP2_VENDOR}/yyjson)
    # Force-include the plugin support umbrella — same as the backend's own
    # runtime compile (xi_script_compiler.hpp `/FIxi/xi_plugin_support.hpp`).
    # Plugin sources (and the scaffold templates) rely on xi::Plugin /
    # XI_PLUGIN_IMPL / xi::Record being present without an explicit #include;
    # idempotent for sources that do include the xi headers themselves.
    # Apply the force-include ONLY to the plugin's own sources (${ARGN}) — a
    # per-source property, so the bundled yyjson.c (C, must not pull in the C++
    # umbrella) is left alone. (Target-level COMPILE_LANGUAGE scoping isn't
    # honoured by the VS generator, hence the per-source form.)
    if(MSVC)
        set_source_files_properties(${ARGN} PROPERTIES
            COMPILE_FLAGS "/FIxi/xi_plugin_support.hpp")
    else()
        set_source_files_properties(${ARGN} PROPERTIES
            COMPILE_FLAGS "-includexi/xi_plugin_support.hpp")
    endif()
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
#
# OpenCV is wired here too: a test that stands up the host image API
# (xi_image_pool.hpp → xi_image.hpp) transitively includes
# <opencv2/core.hpp>, so without it every plugin's test fails with
# `C1083: 'opencv2/core.hpp'`. Mirror xinsp2_add_plugin's OpenCV wiring.
function(xinsp2_add_plugin_test target_name)
    add_executable(${target_name} ${ARGN} ${XINSP2_YYJSON})
    target_include_directories(${target_name} PRIVATE
        ${XINSP2_INCLUDE}
        ${XINSP2_VENDOR}
        ${XINSP2_VENDOR}/yyjson
        ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(${target_name} PRIVATE ${OpenCV_LIBS})
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()
