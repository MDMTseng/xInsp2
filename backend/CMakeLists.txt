cmake_minimum_required(VERSION 3.16)
project(xinsp2_backend CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

add_library(xi_core INTERFACE)
target_include_directories(xi_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

enable_testing()

add_executable(test_xi_core tests/test_xi_core.cpp)
target_link_libraries(test_xi_core PRIVATE xi_core)
if(NOT MSVC)
    target_link_libraries(test_xi_core PRIVATE pthread)
endif()
add_test(NAME xi_core COMMAND test_xi_core)

add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE xi_core)
add_test(NAME protocol COMMAND test_protocol)
set_tests_properties(protocol PROPERTIES
    ENVIRONMENT "XINSP2_FIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/../protocol/fixtures"
)

add_executable(xinsp_backend src/service_main.cpp)
target_link_libraries(xinsp_backend PRIVATE xi_core)
if(WIN32)
    target_link_libraries(xinsp_backend PRIVATE Ws2_32)
endif()
set_target_properties(xinsp_backend PROPERTIES OUTPUT_NAME "xinsp-backend")
