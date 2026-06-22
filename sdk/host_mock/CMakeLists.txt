# xi_run_plugin — headless host-mock CLI. Build once, point it at any plugin DLL.
#
#   set XINSP2_ROOT=C:\path\to\xInsp2
#   cmake -S sdk/host_mock -B sdk/host_mock/build -A x64 -DXINSP2_ROOT=%XINSP2_ROOT%
#   cmake --build sdk/host_mock/build --config Release
#   sdk/host_mock/xi_run_plugin.exe <plugin.dll> --image frame=in.png --def "{...}"
cmake_minimum_required(VERSION 3.16)
project(xi_run_plugin CXX C)
set(CMAKE_CXX_STANDARD 20)

if(NOT DEFINED XINSP2_ROOT AND DEFINED ENV{XINSP2_ROOT})
  set(XINSP2_ROOT $ENV{XINSP2_ROOT})
endif()
# Pulls in the xInsp2 include/vendor vars + finds OpenCV (xinsp2_find_opencv).
include(${XINSP2_ROOT}/sdk/cmake/xinsp2_plugin.cmake)

add_executable(xi_run_plugin xi_run_plugin.cpp ${XINSP2_YYJSON})
target_include_directories(xi_run_plugin PRIVATE
    ${XINSP2_INCLUDE} ${XINSP2_VENDOR} ${XINSP2_VENDOR}/yyjson ${OpenCV_INCLUDE_DIRS})
target_link_libraries(xi_run_plugin PRIVATE ${OpenCV_LIBS})
set_target_properties(xi_run_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR})
