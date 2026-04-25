cmake_minimum_required(VERSION 3.16)
project(xinsp2_backend C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /permissive- /utf-8 /EHa)
else()
    add_compile_options(-Wall -Wextra)
endif()

# JPEG encoder backends (runtime-dispatched by CPU vendor)
option(XINSP2_HAS_IPP        "Link Intel IPP for JPEG encode on Intel CPUs" OFF)
option(XINSP2_HAS_OPENCV     "Link OpenCV (provides cv::imencode and image ops)" OFF)
option(XINSP2_HAS_TURBOJPEG  "Link libjpeg-turbo for direct SIMD JPEG encode" OFF)
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
    find_package(OpenCV REQUIRED COMPONENTS core imgcodecs imgproc)
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
set_tests_properties(protocol PROPERTIES
    ENVIRONMENT "XINSP2_FIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/../protocol/fixtures"
)

add_executable(test_image_pool tests/test_image_pool.cpp)
target_link_libraries(test_image_pool PRIVATE xi_core cjson)
if(NOT MSVC)
    target_link_libraries(test_image_pool PRIVATE pthread)
endif()
add_test(NAME image_pool COMMAND test_image_pool)

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
add_test(NAME ops COMMAND test_ops)

add_executable(xinsp_backend
    src/service_main.cpp
    src/stb_impl.cpp
)
target_link_libraries(xinsp_backend PRIVATE xi_core cjson)
target_include_directories(xinsp_backend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
if(WIN32)
    target_link_libraries(xinsp_backend PRIVATE Ws2_32)
endif()
set_target_properties(xinsp_backend PROPERTIES OUTPUT_NAME "xinsp-backend")

# Headless production runner. Same libs as the server; no WebSocket.
add_executable(xinsp_runner src/runner_main.cpp)
target_link_libraries(xinsp_runner PRIVATE xi_core cjson)
target_include_directories(xinsp_runner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
if(WIN32)
    target_link_libraries(xinsp_runner PRIVATE Ws2_32)
endif()
set_target_properties(xinsp_runner PROPERTIES OUTPUT_NAME "xinsp-runner")

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

# Always-cpp variant for side-by-side comparison.
add_executable(bench_ops_cpp tests/bench_ops.cpp)
target_link_libraries(bench_ops_cpp PRIVATE xi_core)

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
