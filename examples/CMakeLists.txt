cmake_minimum_required(VERSION 3.16)
project(xinsp2_m0_demo CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(demo_async demo_async.cpp)
target_include_directories(demo_async PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../backend/include)

if(NOT MSVC)
    target_link_libraries(demo_async PRIVATE pthread)
endif()
