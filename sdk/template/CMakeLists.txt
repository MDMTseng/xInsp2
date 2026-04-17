cmake_minimum_required(VERSION 3.16)
project(my_plugin CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/W3 /utf-8)
endif()

# Point this at your xInsp2 checkout root.
if(NOT DEFINED XINSP2_ROOT)
    set(XINSP2_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
endif()

add_library(my_plugin SHARED
    my_plugin.cpp
    ${XINSP2_ROOT}/backend/vendor/cJSON.c
)
target_include_directories(my_plugin PRIVATE
    ${XINSP2_ROOT}/backend/include
    ${XINSP2_ROOT}/backend/vendor
)

# Drop the DLL directly into the plugin folder so the host picks it up.
set_target_properties(my_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}
)
