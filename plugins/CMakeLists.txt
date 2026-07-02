cmake_minimum_required(VERSION 3.16)
project(xinsp2_plugins C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    # /EHa (async exceptions) is REQUIRED, not cosmetic — it matches the SDK's own
    # plugin build (sdk/cmake/xinsp2_plugin.cmake) and the backend EXE. The host
    # runs every plugin call under _set_se_translator (xi_seh.hpp), and a source's
    # xi::spawn_worker installs the same translator on its worker thread; MSVC only
    # emits the unwind funclets that make that SEH→C++ translation sound AND run
    # local dtors during unwind under /EHa. Under the default /EHsc the catch still
    # fires but RAII in the faulting frame is skipped, leaking its pool_image
    # handles (1 ImagePool slot per absorbed crash) — and _set_se_translator warns
    # C4535 that it is inoperative.
    add_compile_options(/W3 /utf-8 /EHa)
endif()

# All plugin DLLs are namespaced with an "xi-" prefix (xi-<name>.dll). Set once
# here so every SHARED target below inherits it; each plugin.json "dll" matches.
set(CMAKE_SHARED_LIBRARY_PREFIX "xi-")

set(XI_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../backend/include)
set(XI_VENDOR  ${CMAKE_CURRENT_SOURCE_DIR}/../backend/vendor)

# OpenCV — NO LONGER a mandatory dependency (OQ-9). xi_image.hpp / xi_abi.hpp
# are now OpenCV-free; the cv:: bridge lives in the opt-in <xi/xi_cv.hpp>. None
# of the in-tree plugins below do any CV, so they build with OpenCV absent.
# A plugin that wants cv:: just #includes <xi/xi_cv.hpp> and adds OpenCV to its
# own target (find_package(OpenCV ...) + target_link_libraries) — see the
# example plugins under examples/*/plugins for the pattern.

# yyjson — Record's DOM/codec and the only JSON lib plugins use. Every plugin
# includes xi_record (via xi_abi), so
# link + include it for ALL plugin targets defined below (directory scope).
add_library(yyjson_plugin STATIC ${XI_VENDOR}/yyjson/yyjson.c)
target_include_directories(yyjson_plugin PUBLIC ${XI_VENDOR}/yyjson)
include_directories(${XI_VENDOR}/yyjson)
link_libraries(yyjson_plugin)

# mock_camera
add_library(mock_camera SHARED mock_camera/mock_camera.cpp)
target_include_directories(mock_camera PRIVATE ${XI_INCLUDE} ${XI_VENDOR})
set_target_properties(mock_camera PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/mock_camera
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/mock_camera
)

# expose — the official script data-out surface (post-VAR): keeps the latest
# record per channel, JPEG-encodes images via the host cache + pushes the whole
# record as one atomic XEX1 binary frame for SUBSCRIBED channels. Script API:
# <xi/xi_expose.hpp>. See examples/preview_sink_demo.
add_library(expose SHARED expose/src/expose.cpp)
target_include_directories(expose PRIVATE ${XI_INCLUDE} ${XI_VENDOR})
set_target_properties(expose PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/expose
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/expose
)

# cache — buffers records in a ring and re-emits a buffered one on exchange
# (the plugin-side record/replay store; drives the hot-param re-inspect loop).
add_library(cache SHARED cache/src/cache.cpp)
target_include_directories(cache PRIVATE ${XI_INCLUDE} ${XI_VENDOR})
set_target_properties(cache PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/cache
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/cache
)

# config_swap_probe — reference plugin for the orchestrator config-swap design
# (double-slot prepare/commit via exchange; see
# docs/roadmap/config-bundles-and-orchestration.md)
add_library(config_swap_probe SHARED config_swap_probe/config_swap_probe.cpp)
target_include_directories(config_swap_probe PRIVATE ${XI_INCLUDE} ${XI_VENDOR})
set_target_properties(config_swap_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/config_swap_probe
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/config_swap_probe
)

# synced_stereo — emits left+right frames under one trigger ID
add_library(synced_stereo SHARED synced_stereo/synced_stereo.cpp)
target_include_directories(synced_stereo PRIVATE ${XI_INCLUDE} ${XI_VENDOR})
set_target_properties(synced_stereo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/synced_stereo
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/synced_stereo
)

# data_output
add_library(data_output SHARED data_output/data_output.cpp)
target_include_directories(data_output PRIVATE ${XI_INCLUDE})
set_target_properties(data_output PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/data_output
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/data_output
)

# blob_analysis
add_library(blob_analysis SHARED blob_analysis/blob_analysis.cpp)
target_include_directories(blob_analysis PRIVATE ${XI_INCLUDE} ${XI_VENDOR})

# json_source
add_library(json_source SHARED json_source/json_source.cpp)
target_include_directories(json_source PRIVATE ${XI_INCLUDE} ${XI_VENDOR})
set_target_properties(json_source PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/json_source
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/json_source
)

# record_save (uses stb_image_write for BMP output)
add_library(record_save SHARED
    record_save/record_save.cpp
    ../backend/src/stb_impl.cpp
)
target_include_directories(record_save PRIVATE ${XI_INCLUDE} ${XI_VENDOR})
set_target_properties(record_save PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/record_save
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/record_save
)
set_target_properties(blob_analysis PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/blob_analysis
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/blob_analysis
)

# --- Plugin data-contract tests (docs/new_gen/02-plugin-data-contract.md) ----
# Developer-side test exes: load the built plugin DLL via LoadLibrary and assert
# the contract behaviour (structured missing-input failures, schema skew, and the
# builder/extractor happy path). Plugins are trusted, so these are optional — but
# they keep the two most-copied exemplars honest. yyjson is already link_libraries'd
# for the whole directory (above), so the Record/Json code links in.
enable_testing()

# blob_analysis contract test — needs the host ImagePool (make_host_api) to build
# a real pool-backed gray input, so its process() runs exactly as in the backend.
add_executable(blob_analysis_test blob_analysis/tests/test_blob_analysis.cpp)
target_include_directories(blob_analysis_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/blob_analysis)
target_compile_definitions(blob_analysis_test PRIVATE
    BLOB_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/blob_analysis/xi-blob_analysis.dll")
add_dependencies(blob_analysis_test blob_analysis)
add_test(NAME blob_analysis_test COMMAND blob_analysis_test)

# mock_camera contract test — config/command surface only (source plugin).
add_executable(mock_camera_test mock_camera/tests/test_mock_camera.cpp)
target_include_directories(mock_camera_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/mock_camera)
target_compile_definitions(mock_camera_test PRIVATE
    MOCK_CAMERA_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/mock_camera/xi-mock_camera.dll")
add_dependencies(mock_camera_test mock_camera)
add_test(NAME mock_camera_test COMMAND mock_camera_test)

# XEX1 binary golden fixtures — drives the SAME encoder the expose plugin ships
# (expose/src/xex1_encode.hpp) and byte-compares its output against the committed
# protocol/fixtures/binary/*.bin. The JS + Python decoder tests decode those same
# goldens, so the three hand-rolled msgpack codecs are held in lockstep. Run with
# --regen to (re)write the goldens + manifest after an intentional frame change.
add_executable(xex1_fixtures_test expose/tests/test_xex1_fixtures.cpp)
target_include_directories(xex1_fixtures_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/expose/src)
add_test(NAME xex1_fixtures COMMAND xex1_fixtures_test)
set_tests_properties(xex1_fixtures PROPERTIES
    ENVIRONMENT "XINSP2_FIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/../protocol/fixtures")
