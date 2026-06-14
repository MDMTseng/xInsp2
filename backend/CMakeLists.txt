cmake_minimum_required(VERSION 3.16)

# ---- Single source of truth for the project version --------------------
# Drives: cmd:version response, hello event, --version CLI, the SDK
# template's plugin.json, VS Code extension package.json (manually
# kept in sync), and the release zip filename.
set(XINSP2_VERSION "0.2.0")
project(xinsp2_backend VERSION ${XINSP2_VERSION} LANGUAGES C CXX)

# Embed git short hash if available (release builds will set XINSP2_COMMIT
# explicitly via -DXINSP2_COMMIT=...); fallback to "dev".
if(NOT DEFINED XINSP2_COMMIT)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..
            OUTPUT_VARIABLE _git_hash
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_git_hash)
            set(XINSP2_COMMIT "${_git_hash}")
        endif()
    endif()
    if(NOT DEFINED XINSP2_COMMIT)
        set(XINSP2_COMMIT "dev")
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /permissive- /utf-8 /EHa)
else()
    add_compile_options(-Wall -Wextra)
endif()

# OpenCV is mandatory (image operators); IPP / turbojpeg stay opt-in
# accelerators for the JPEG-encode path only.
option(XINSP2_HAS_IPP        "Link Intel IPP for JPEG encode acceleration" OFF)
option(XINSP2_HAS_TURBOJPEG  "Link libjpeg-turbo for direct SIMD JPEG encode" OFF)
# Default install root: C:\libjpeg-turbo64 (Windows) or /opt/libjpeg-turbo (Linux)
set(TURBOJPEG_ROOT "C:/libjpeg-turbo64" CACHE PATH "libjpeg-turbo install root")

add_library(cjson STATIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/cJSON.c)
target_include_directories(cjson PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor)

# CWPack — MessagePack codec for the Record wire (xi_msgpack.hpp). Shipped lib,
# linked into the backend and (mirroring cjson.lib) into JIT-compiled plugins.
add_library(cwpack STATIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/cwpack/cwpack.c)
target_include_directories(cwpack PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/cwpack)

add_library(xi_core INTERFACE)
target_include_directories(xi_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)
# xi_abi.hpp / xi_use.hpp pull in xi_msgpack.hpp (CWPack) — every xi_core user
# links it. (cjson stays listed explicitly on each target.)
target_link_libraries(xi_core INTERFACE cwpack)
# xi.hpp / xi_plugin_support.hpp pull in <opencv2/opencv.hpp>, so every
# target that uses xi_core needs OpenCV's include + libs visible.
# OpenCV is configured immediately below.

enable_testing()

# Resolve optional encoder libs ONCE so every target that needs them
# (xinsp_backend, xinsp_runner, bench_jpeg) shares the same lookup.
# OpenCV: mandatory. Auto-detect typical Windows install locations if
# OpenCV_DIR isn't already set.
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
target_include_directories(xi_core INTERFACE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(xi_core INTERFACE ${OpenCV_LIBS})
if(XINSP2_HAS_IPP)
    # Default install layout: C:/Intel/ipp/<ver>/{include,lib,bin}
    # Override with -DIPP_ROOT=... or env IPP_ROOT.
    if(NOT DEFINED IPP_ROOT AND DEFINED ENV{IPP_ROOT})
        set(IPP_ROOT $ENV{IPP_ROOT})
    endif()
    if(NOT DEFINED IPP_ROOT)
        file(GLOB _ipp_versions "C:/Intel/ipp/*")
        foreach(_v ${_ipp_versions})
            if(EXISTS "${_v}/include/ippi.h")
                set(IPP_ROOT "${_v}")
                break()
            endif()
        endforeach()
    endif()
    find_path(IPP_INCLUDE_DIR ippi.h HINTS ${IPP_ROOT}/include)
    find_library(IPP_CORE_LIB ippcore HINTS ${IPP_ROOT}/lib)
    find_library(IPP_I_LIB    ippi    HINTS ${IPP_ROOT}/lib)
    find_library(IPP_CV_LIB   ippcv   HINTS ${IPP_ROOT}/lib)
    find_library(IPP_CC_LIB   ippcc   HINTS ${IPP_ROOT}/lib)
    if(NOT (IPP_INCLUDE_DIR AND IPP_CORE_LIB))
        message(WARNING "XINSP2_HAS_IPP=ON but IPP not found at ${IPP_ROOT}; set -DIPP_ROOT=...")
    endif()
endif()
if(XINSP2_HAS_TURBOJPEG)
    find_path(TURBOJPEG_INCLUDE_DIR turbojpeg.h
        HINTS ${TURBOJPEG_ROOT}/include /usr/include /usr/local/include)
    find_library(TURBOJPEG_LIB NAMES turbojpeg
        HINTS ${TURBOJPEG_ROOT}/lib /usr/lib /usr/local/lib)
    if(NOT (TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIB))
        message(WARNING "XINSP2_HAS_TURBOJPEG=ON but libjpeg-turbo not found at ${TURBOJPEG_ROOT}")
    endif()
endif()

add_executable(test_xi_core tests/test_xi_core.cpp)
target_link_libraries(test_xi_core PRIVATE xi_core cjson)
if(NOT MSVC)
    target_link_libraries(test_xi_core PRIVATE pthread)
endif()
add_test(NAME xi_core COMMAND test_xi_core)

add_executable(test_record tests/test_record.cpp)
target_link_libraries(test_record PRIVATE xi_core cjson)
add_test(NAME record COMMAND test_record)

add_executable(test_msgpack tests/test_msgpack.cpp)
target_link_libraries(test_msgpack PRIVATE xi_core cjson cwpack)
add_test(NAME msgpack COMMAND test_msgpack)

add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE xi_core cjson)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_diagnostics tests/test_diagnostics.cpp)
target_link_libraries(test_diagnostics PRIVATE xi_core cjson)
add_test(NAME diagnostics COMMAND test_diagnostics)

# SHM / worker-process / script-runner tests removed 2026-05 with the
# FE/BE in-process split (no more SHM region or worker processes).
set_tests_properties(protocol PROPERTIES
    ENVIRONMENT "XINSP2_FIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/../protocol/fixtures"
)

add_executable(test_image_pool tests/test_image_pool.cpp)
target_link_libraries(test_image_pool PRIVATE xi_core cjson)
if(NOT MSVC)
    target_link_libraries(test_image_pool PRIVATE pthread)
endif()
add_test(NAME image_pool COMMAND test_image_pool)

add_executable(test_image_pool_stress tests/test_image_pool_stress.cpp)
target_link_libraries(test_image_pool_stress PRIVATE xi_core cjson)
if(NOT MSVC)
    target_link_libraries(test_image_pool_stress PRIVATE pthread)
endif()
add_test(NAME image_pool_stress COMMAND test_image_pool_stress)

# Throughput micro-benchmark (not a ctest — run manually to measure alloc cost).
add_executable(bench_image_pool tests/bench_image_pool.cpp)
target_link_libraries(bench_image_pool PRIVATE xi_core cjson)
if(NOT MSVC)
    target_link_libraries(bench_image_pool PRIVATE pthread)
endif()

add_executable(test_sha256 tests/test_sha256.cpp)
target_link_libraries(test_sha256 PRIVATE xi_core)
add_test(NAME sha256 COMMAND test_sha256)

# SafeStateSink unit (FE safe-state seam). Header-only under test; links xi_core
# only for the include path. See docs/design/fe-be-split-test-plan.md §1.
add_executable(test_safe_state tests/test_safe_state.cpp)
target_link_libraries(test_safe_state PRIVATE xi_core)
add_test(NAME safe_state COMMAND test_safe_state)

# QA team-loop units (FE/BE split hardening). See docs/design/fe-test-rounds/.
# qa_fault/qa_race pin the respawn cap + sliding-window math; qa_edge pins the
# crash-report parser (xi_crash_report.hpp) against crafted fixtures.
add_executable(test_qa_fault tests/test_qa_fault.cpp)
target_link_libraries(test_qa_fault PRIVATE xi_core)
add_test(NAME qa_fault COMMAND test_qa_fault)

add_executable(test_qa_race tests/test_qa_race.cpp)
target_link_libraries(test_qa_race PRIVATE xi_core)
add_test(NAME qa_race COMMAND test_qa_race)

add_executable(test_qa_edge tests/test_qa_edge.cpp)
target_link_libraries(test_qa_edge PRIVATE xi_core cjson)
add_test(NAME qa_edge COMMAND test_qa_edge)

# Phase G (#92) stress/fuzz: hammers the respawn-cap + safe-state core with
# degenerate caps, the exact reset boundary, recover-then-recur cycles, a
# 200k-step equivalence fuzz, and high-volume emission. See docs/design/
# fe-be-split-test-plan.md "Phase G".
add_executable(test_qa_stress tests/test_qa_stress.cpp)
target_link_libraries(test_qa_stress PRIVATE xi_core)
add_test(NAME qa_stress COMMAND test_qa_stress)

add_executable(xinsp_backend
    src/service_main.cpp
    src/stb_impl.cpp
    src/xi_image_io.cpp
)
target_link_libraries(xinsp_backend PRIVATE xi_core cjson)
target_include_directories(xinsp_backend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
if(WIN32)
    target_link_libraries(xinsp_backend PRIVATE Ws2_32)
endif()
set_target_properties(xinsp_backend PROPERTIES OUTPUT_NAME "xinsp-backend")
target_compile_definitions(xinsp_backend PRIVATE
    XINSP2_VERSION="${XINSP2_VERSION}"
    XINSP2_COMMIT="${XINSP2_COMMIT}")

# xinsp_worker / xinsp_script_runner / evil_worker removed 2026-05:
# the FE/BE in-process split means plugins and scripts run inside the
# backend, so there are no worker / script-runner processes to spawn.

# Headless production runner. Same libs as the server; no WebSocket.
add_executable(xinsp_runner src/runner_main.cpp src/stb_impl.cpp src/xi_image_io.cpp)
target_link_libraries(xinsp_runner PRIVATE xi_core cjson)
target_include_directories(xinsp_runner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
if(WIN32)
    target_link_libraries(xinsp_runner PRIVATE Ws2_32)
endif()
set_target_properties(xinsp_runner PROPERTIES OUTPUT_NAME "xinsp-runner")
target_compile_definitions(xinsp_runner PRIVATE
    XINSP2_VERSION="${XINSP2_VERSION}"
    XINSP2_COMMIT="${XINSP2_COMMIT}")

# Frontend supervisor. Spawns + monitors xinsp-backend.exe, drives the line to a
# safe state on backend death, respawns it (rate-limited). Links NEITHER xi_core
# NOR OpenCV — it only needs the portable xi_safe_state.hpp header + cJSON + the
# Win32 process/socket APIs, so it carries no accelerator-DLL dependency and is
# excluded from _xinsp_deploy_dlls below.
add_executable(xinsp_fe src/fe_main.cpp)
target_link_libraries(xinsp_fe PRIVATE cjson)
target_include_directories(xinsp_fe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
if(WIN32)
    target_link_libraries(xinsp_fe PRIVATE Ws2_32)
endif()
set_target_properties(xinsp_fe PROPERTIES OUTPUT_NAME "xinsp-fe")
target_compile_definitions(xinsp_fe PRIVATE
    XINSP2_VERSION="${XINSP2_VERSION}"
    XINSP2_COMMIT="${XINSP2_COMMIT}")

# (xinsp-comms gateway removed — PLC I/O is now a plugin concern; the FE's
# safe-state sink + host->set_safe_state cover the BE-crash case. See
# docs/design/comms-gateway.md.)

# JPEG-encoder benchmark — quantifies the IPP/OpenCV/stb dispatch.
# Not run as a test (no pass/fail); invoke manually for numbers.
add_executable(bench_jpeg tests/bench_jpeg.cpp src/stb_impl.cpp)
target_link_libraries(bench_jpeg PRIVATE xi_core cjson)
if(XINSP2_HAS_IPP AND IPP_INCLUDE_DIR AND IPP_CORE_LIB)
    target_compile_definitions(bench_jpeg PRIVATE XINSP2_HAS_IPP=1)
    target_include_directories(bench_jpeg PRIVATE ${IPP_INCLUDE_DIR})
    target_link_libraries(bench_jpeg PRIVATE
        ${IPP_CORE_LIB} ${IPP_I_LIB} ${IPP_CV_LIB} ${IPP_CC_LIB})
endif()
target_compile_definitions(bench_jpeg PRIVATE XINSP2_HAS_OPENCV=1)
# OpenCV include + libs come via xi_core's INTERFACE.
if(XINSP2_HAS_TURBOJPEG AND TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIB)
    target_compile_definitions(bench_jpeg PRIVATE XINSP2_HAS_TURBOJPEG=1)
    target_include_directories(bench_jpeg PRIVATE ${TURBOJPEG_INCLUDE_DIR})
    target_link_libraries(bench_jpeg PRIVATE ${TURBOJPEG_LIB})
endif()

# Record serialization micro-bench: cJSON vs yyjson vs MPack vs CWPack.
# The serde libs are bench-only third-party sources, gitignored + fetched on
# demand (see tests/serde_vendor/FETCH.md). Target is skipped if they're absent
# so a clean checkout still configures. Run manually, not a ctest.
set(_serde ${CMAKE_CURRENT_SOURCE_DIR}/tests/serde_vendor)
if(EXISTS ${_serde}/yyjson/yyjson.c AND EXISTS ${_serde}/mpack/mpack.c)
    add_executable(bench_record
        tests/bench_record.cpp
        ${_serde}/yyjson/yyjson.c
        ${_serde}/mpack/mpack.c
        ${_serde}/cwpack/cwpack.c)
    target_link_libraries(bench_record PRIVATE cjson)
    target_include_directories(bench_record PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor
        ${_serde}/yyjson ${_serde}/mpack ${_serde}/cwpack)
endif()

if(XINSP2_HAS_IPP AND IPP_INCLUDE_DIR AND IPP_CORE_LIB)
    target_compile_definitions(xinsp_backend PRIVATE XINSP2_HAS_IPP=1)
    target_include_directories(xinsp_backend PRIVATE ${IPP_INCLUDE_DIR})
    target_link_libraries(xinsp_backend PRIVATE
        ${IPP_CORE_LIB} ${IPP_I_LIB} ${IPP_CV_LIB} ${IPP_CC_LIB})
endif()

target_compile_definitions(xinsp_backend PRIVATE XINSP2_HAS_OPENCV=1)
# OpenCV include + libs come via xi_core's INTERFACE.

if(XINSP2_HAS_TURBOJPEG AND TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIB)
    target_compile_definitions(xinsp_backend PRIVATE XINSP2_HAS_TURBOJPEG=1)
    target_include_directories(xinsp_backend PRIVATE ${TURBOJPEG_INCLUDE_DIR})
    target_link_libraries(xinsp_backend PRIVATE ${TURBOJPEG_LIB})
endif()

# ---- Runtime DLL deployment --------------------------------------------
# Copy accelerator DLLs next to the exe so end users / CI don't need a
# pre-mangled PATH. We do this for xinsp_backend and xinsp_runner.
function(_xinsp_deploy_dlls tgt)
    set(_dll_files "")
    # Pre-built OpenCV ships its DLLs in <root>/x64/<vc>/bin.
    foreach(_vc "vc16" "vc17" "vc14")
        set(_bin "${OpenCV_DIR}/../bin")
        if(EXISTS "${_bin}")
            file(GLOB _cv_dlls "${_bin}/opencv_world*.dll" "${_bin}/opencv_videoio_*.dll")
            list(APPEND _dll_files ${_cv_dlls})
            break()
        endif()
    endforeach()
    if(XINSP2_HAS_TURBOJPEG AND TURBOJPEG_LIB)
        get_filename_component(_tj_dir "${TURBOJPEG_LIB}" DIRECTORY)
        get_filename_component(_tj_root "${_tj_dir}" DIRECTORY)
        if(EXISTS "${_tj_root}/bin/turbojpeg.dll")
            list(APPEND _dll_files "${_tj_root}/bin/turbojpeg.dll")
        endif()
    endif()
    if(XINSP2_HAS_IPP AND IPP_INCLUDE_DIR)
        get_filename_component(_ipp_root "${IPP_INCLUDE_DIR}" DIRECTORY)
        if(EXISTS "${_ipp_root}/bin")
            file(GLOB _ipp_dlls "${_ipp_root}/bin/ipp*.dll")
            list(APPEND _dll_files ${_ipp_dlls})
        endif()
    endif()
    foreach(_dll ${_dll_files})
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "$<TARGET_FILE_DIR:${tgt}>/")
    endforeach()
endfunction()
_xinsp_deploy_dlls(xinsp_backend)
_xinsp_deploy_dlls(xinsp_runner)
