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

# JPEG encoder + image-op backends.
# Defaults: OpenCV is auto-detected (it's the default ops backend below
# the IPP layer); IPP and turbojpeg stay opt-in because they're not
# universally installed.
option(XINSP2_HAS_IPP        "Link Intel IPP for JPEG encode + image ops" OFF)
option(XINSP2_HAS_TURBOJPEG  "Link libjpeg-turbo for direct SIMD JPEG encode" OFF)
option(XINSP2_HAS_OPENCV     "Link OpenCV (default ops backend; auto-detected)" ON)
# Default install root: C:\libjpeg-turbo64 (Windows) or /opt/libjpeg-turbo (Linux)
set(TURBOJPEG_ROOT "C:/libjpeg-turbo64" CACHE PATH "libjpeg-turbo install root")

add_library(cjson STATIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/cJSON.c)
target_include_directories(cjson PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor)

add_library(xi_core INTERFACE)
target_include_directories(xi_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)

enable_testing()

# Resolve optional encoder libs ONCE so every target that needs them
# (xinsp_backend, xinsp_runner, bench_jpeg) shares the same lookup.
if(XINSP2_HAS_OPENCV)
    # Auto-detect typical Windows install locations if OpenCV_DIR not set.
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
    find_package(OpenCV QUIET COMPONENTS core imgcodecs imgproc)
    if(NOT OpenCV_FOUND)
        message(WARNING "XINSP2_HAS_OPENCV=ON but OpenCV not found; ops fall back to portable C++. Pass -DOpenCV_DIR=... or install OpenCV.")
    endif()
endif()
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

add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE xi_core cjson)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_diagnostics tests/test_diagnostics.cpp)
target_link_libraries(test_diagnostics PRIVATE xi_core cjson)
add_test(NAME diagnostics COMMAND test_diagnostics)

add_executable(test_xi_shm tests/test_xi_shm.cpp)
target_link_libraries(test_xi_shm PRIVATE xi_core cjson)
add_test(NAME xi_shm COMMAND test_xi_shm)

# Worker E2E: tiny test plugin DLL + a process driver. Validates the
# zero-copy contract end-to-end across the backend/worker boundary.
add_library(test_worker_plugin SHARED tests/test_worker_plugin.cpp)
target_link_libraries(test_worker_plugin PRIVATE xi_core)
set_target_properties(test_worker_plugin PROPERTIES PREFIX "")  # no "lib" prefix

add_executable(test_worker tests/test_worker.cpp)
target_link_libraries(test_worker PRIVATE xi_core cjson)
add_dependencies(test_worker xinsp_worker test_worker_plugin)
add_test(NAME worker COMMAND test_worker)

add_executable(test_isolated_instance tests/test_isolated_instance.cpp)
target_link_libraries(test_isolated_instance PRIVATE xi_core cjson)
add_dependencies(test_isolated_instance xinsp_worker test_worker_plugin)
add_test(NAME isolated_instance COMMAND test_isolated_instance)

add_executable(test_worker_respawn tests/test_worker_respawn.cpp)
target_link_libraries(test_worker_respawn PRIVATE xi_core cjson)
add_dependencies(test_worker_respawn xinsp_worker test_worker_plugin)
add_test(NAME worker_respawn COMMAND test_worker_respawn)

# Phase 3 minimum: script-runner + tiny test "script" DLL + driver.
add_library(test_script_runner_dll SHARED tests/test_script_runner_dll.cpp)
target_link_libraries(test_script_runner_dll PRIVATE xi_core)
set_target_properties(test_script_runner_dll PROPERTIES PREFIX "")

add_executable(test_script_runner tests/test_script_runner.cpp)
target_link_libraries(test_script_runner PRIVATE xi_core cjson)
add_dependencies(test_script_runner xinsp_script_runner test_script_runner_dll)
add_test(NAME script_runner COMMAND test_script_runner)

add_executable(test_script_process_adapter tests/test_script_process_adapter.cpp)
target_link_libraries(test_script_process_adapter PRIVATE xi_core cjson)
add_dependencies(test_script_process_adapter xinsp_script_runner test_script_runner_dll)
add_test(NAME script_process_adapter COMMAND test_script_process_adapter)

add_executable(test_script_runner_respawn tests/test_script_runner_respawn.cpp)
target_link_libraries(test_script_runner_respawn PRIVATE xi_core cjson)
add_dependencies(test_script_runner_respawn xinsp_script_runner test_script_runner_dll)
add_test(NAME script_runner_respawn COMMAND test_script_runner_respawn)

add_library(test_hang_plugin SHARED tests/test_hang_plugin.cpp)
target_link_libraries(test_hang_plugin PRIVATE xi_core)
set_target_properties(test_hang_plugin PROPERTIES PREFIX "")

add_executable(test_worker_timeout tests/test_worker_timeout.cpp)
target_link_libraries(test_worker_timeout PRIVATE xi_core cjson)
add_dependencies(test_worker_timeout xinsp_worker test_hang_plugin)
add_test(NAME worker_timeout COMMAND test_worker_timeout)

add_executable(test_shm_ipc_edges tests/test_shm_ipc_edges.cpp)
target_link_libraries(test_shm_ipc_edges PRIVATE xi_core cjson)
add_test(NAME shm_ipc_edges COMMAND test_shm_ipc_edges)
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

add_executable(test_ops tests/test_ops.cpp)
target_link_libraries(test_ops PRIVATE xi_core)
if(NOT MSVC)
    target_link_libraries(test_ops PRIVATE pthread)
endif()
if(XINSP2_HAS_IPP AND IPP_INCLUDE_DIR AND IPP_CORE_LIB)
    target_compile_definitions(test_ops PRIVATE XINSP2_HAS_IPP=1)
    target_include_directories(test_ops PRIVATE ${IPP_INCLUDE_DIR})
    target_link_libraries(test_ops PRIVATE
        ${IPP_CORE_LIB} ${IPP_I_LIB} ${IPP_CV_LIB} ${IPP_CC_LIB})
endif()
if(XINSP2_HAS_OPENCV AND OpenCV_FOUND)
    target_compile_definitions(test_ops PRIVATE XINSP2_HAS_OPENCV=1)
    target_include_directories(test_ops PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(test_ops PRIVATE ${OpenCV_LIBS})
endif()
add_test(NAME ops COMMAND test_ops)

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

# Headless production runner. Same libs as the server; no WebSocket.
add_executable(xinsp_worker src/worker_main.cpp src/stb_impl.cpp src/xi_image_io.cpp)
target_link_libraries(xinsp_worker PRIVATE xi_core cjson)
target_include_directories(xinsp_worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
set_target_properties(xinsp_worker PROPERTIES OUTPUT_NAME "xinsp-worker")

add_executable(xinsp_script_runner src/script_runner_main.cpp src/stb_impl.cpp src/xi_image_io.cpp)
target_link_libraries(xinsp_script_runner PRIVATE xi_core cjson)
target_include_directories(xinsp_script_runner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
set_target_properties(xinsp_script_runner PROPERTIES OUTPUT_NAME "xinsp-script-runner")

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
if(XINSP2_HAS_OPENCV AND OpenCV_FOUND)
    target_compile_definitions(bench_jpeg PRIVATE XINSP2_HAS_OPENCV=1)
    target_include_directories(bench_jpeg PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(bench_jpeg PRIVATE ${OpenCV_LIBS})
endif()
if(XINSP2_HAS_TURBOJPEG AND TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIB)
    target_compile_definitions(bench_jpeg PRIVATE XINSP2_HAS_TURBOJPEG=1)
    target_include_directories(bench_jpeg PRIVATE ${TURBOJPEG_INCLUDE_DIR})
    target_link_libraries(bench_jpeg PRIVATE ${TURBOJPEG_LIB})
endif()

# xi::ops benchmark — toGray / threshold / gaussian / sobel / erode / dilate
add_executable(bench_ops tests/bench_ops.cpp)
target_link_libraries(bench_ops PRIVATE xi_core)
if(XINSP2_HAS_IPP AND IPP_INCLUDE_DIR AND IPP_CORE_LIB)
    target_compile_definitions(bench_ops PRIVATE XINSP2_HAS_IPP=1)
    target_include_directories(bench_ops PRIVATE ${IPP_INCLUDE_DIR})
    target_link_libraries(bench_ops PRIVATE
        ${IPP_CORE_LIB} ${IPP_I_LIB} ${IPP_CV_LIB} ${IPP_CC_LIB})
endif()
if(XINSP2_HAS_OPENCV AND OpenCV_FOUND)
    target_compile_definitions(bench_ops PRIVATE XINSP2_HAS_OPENCV=1)
    target_include_directories(bench_ops PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(bench_ops PRIVATE ${OpenCV_LIBS})
endif()

# Always-cpp variant for side-by-side comparison.
add_executable(bench_ops_cpp tests/bench_ops.cpp)
target_link_libraries(bench_ops_cpp PRIVATE xi_core)

# OpenCV-only variant (no IPP).
if(XINSP2_HAS_OPENCV AND OpenCV_FOUND)
    add_executable(bench_ops_cv tests/bench_ops.cpp)
    target_link_libraries(bench_ops_cv PRIVATE xi_core)
    target_compile_definitions(bench_ops_cv PRIVATE XINSP2_HAS_OPENCV=1)
    target_include_directories(bench_ops_cv PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(bench_ops_cv PRIVATE ${OpenCV_LIBS})
endif()

if(XINSP2_HAS_IPP AND IPP_INCLUDE_DIR AND IPP_CORE_LIB)
    target_compile_definitions(xinsp_backend PRIVATE XINSP2_HAS_IPP=1)
    target_include_directories(xinsp_backend PRIVATE ${IPP_INCLUDE_DIR})
    target_link_libraries(xinsp_backend PRIVATE
        ${IPP_CORE_LIB} ${IPP_I_LIB} ${IPP_CV_LIB} ${IPP_CC_LIB})
endif()

if(XINSP2_HAS_OPENCV AND OpenCV_FOUND)
    target_compile_definitions(xinsp_backend PRIVATE XINSP2_HAS_OPENCV=1)
    target_include_directories(xinsp_backend PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(xinsp_backend PRIVATE ${OpenCV_LIBS})
endif()

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
    if(XINSP2_HAS_OPENCV AND OpenCV_FOUND)
        # Pre-built OpenCV ships its DLLs in <root>/x64/<vc>/bin.
        foreach(_vc "vc16" "vc17" "vc14")
            set(_bin "${OpenCV_DIR}/../bin")
            if(EXISTS "${_bin}")
                file(GLOB _cv_dlls "${_bin}/opencv_world*.dll" "${_bin}/opencv_videoio_*.dll")
                list(APPEND _dll_files ${_cv_dlls})
                break()
            endif()
        endforeach()
    endif()
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
