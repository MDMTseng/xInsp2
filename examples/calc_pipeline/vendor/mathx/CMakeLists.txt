# mathx — built as a static lib and linked into the adder plugin. Stands in for
# any external dependency you'd pull in from outside the plugin folder.
add_library(mathx STATIC mathx.cpp)
target_include_directories(mathx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_property(TARGET mathx PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
