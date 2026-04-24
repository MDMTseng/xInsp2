cmake_minimum_required(VERSION 3.16)
project(counter CXX C)
set(CMAKE_CXX_STANDARD 20)

if(NOT DEFINED XINSP2_ROOT)
    set(XINSP2_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
endif()
include(${XINSP2_ROOT}/sdk/cmake/xinsp2_plugin.cmake)

xinsp2_add_plugin(counter counter.cpp)

# Developer-side test binary. Runs baseline + custom tests. Passing this
# writes cert.json so the backend skips re-testing on first load.
xinsp2_add_plugin_test(counter_test tests/test_counter.cpp)
target_compile_definitions(counter_test PRIVATE
    COUNTER_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/counter.dll")
add_dependencies(counter_test counter)

enable_testing()
add_test(NAME counter_test COMMAND counter_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
