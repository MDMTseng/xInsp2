cmake_minimum_required(VERSION 3.16)
project(counter CXX C)
set(CMAKE_CXX_STANDARD 20)
if(MSVC)
    add_compile_options(/W3 /utf-8)
endif()

if(NOT DEFINED XINSP2_ROOT)
    set(XINSP2_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
endif()

add_library(counter SHARED counter.cpp ${XINSP2_ROOT}/backend/vendor/cJSON.c)
target_include_directories(counter PRIVATE
    ${XINSP2_ROOT}/backend/include
    ${XINSP2_ROOT}/backend/vendor)
set_target_properties(counter PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR})

# Developer-side test binary. Runs baseline + custom tests. Passing this
# writes cert.json so the backend skips re-testing on first load.
add_executable(counter_test tests/test_counter.cpp
    ${XINSP2_ROOT}/backend/vendor/cJSON.c)
target_include_directories(counter_test PRIVATE
    ${XINSP2_ROOT}/backend/include
    ${XINSP2_ROOT}/backend/vendor)
target_compile_definitions(counter_test PRIVATE
    COUNTER_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/counter.dll")
set_target_properties(counter_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(counter_test counter)

enable_testing()
add_test(NAME counter_test COMMAND counter_test)
