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
option(XINSP2_HAS_IPP    "Link Intel IPP for JPEG encode on Intel CPUs" OFF)
option(XINSP2_HAS_OPENCV "Link OpenCV for JPEG encode on non-Intel CPUs" OFF)

add_library(cjson STATIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/cJSON.c)
target_include_directories(cjson PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor)

add_library(xi_core INTERFACE)
target_include_directories(xi_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)

enable_testing()

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

if(XINSP2_HAS_IPP)
    target_compile_definitions(xinsp_backend PRIVATE XINSP2_HAS_IPP=1)
    # User must set IPP_ROOT or CMAKE_PREFIX_PATH to find IPP
    find_path(IPP_INCLUDE_DIR ippi.h HINTS $ENV{IPP_ROOT}/include)
    find_library(IPP_CORE_LIB ippcore HINTS $ENV{IPP_ROOT}/lib)
    find_library(IPP_I_LIB ippi HINTS $ENV{IPP_ROOT}/lib)
    find_library(IPP_J_LIB ippj HINTS $ENV{IPP_ROOT}/lib)
    if(IPP_INCLUDE_DIR AND IPP_CORE_LIB)
        target_include_directories(xinsp_backend PRIVATE ${IPP_INCLUDE_DIR})
        target_link_libraries(xinsp_backend PRIVATE ${IPP_CORE_LIB} ${IPP_I_LIB} ${IPP_J_LIB})
    else()
        message(WARNING "XINSP2_HAS_IPP=ON but IPP not found — set IPP_ROOT")
    endif()
endif()

if(XINSP2_HAS_OPENCV)
    target_compile_definitions(xinsp_backend PRIVATE XINSP2_HAS_OPENCV=1)
    find_package(OpenCV REQUIRED COMPONENTS core imgcodecs imgproc)
    target_link_libraries(xinsp_backend PRIVATE ${OpenCV_LIBS})
endif()
