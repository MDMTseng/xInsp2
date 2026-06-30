# perf_gate.cmake — run a benchmark in --gate mode, parse its INTEGER timing
# metrics, compare each against a recorded baseline, and FAIL the test if any
# metric regresses by more than THRESHOLD_PCT percent. Slower-is-worse: every
# GATE metric is a time (ns/us), so a larger measured value is a regression.
#
# Invoked from ctest as:
#   cmake -DBENCH_EXE=<exe> -DBASELINE=<file> [-DTHRESHOLD_PCT=25]
#         [-DUPDATE_BASELINE=ON] -P perf_gate.cmake
#
# Metrics are emitted by the bench as integer-valued lines:
#   GATE <metric_name> <integer>
# so the comparison uses pure integer CMake math (no float arithmetic):
#   regression  <=>  measured > baseline * (100 + THRESHOLD_PCT) / 100
#
# UPDATE_BASELINE=ON rewrites the baseline from this run instead of comparing —
# how the committed baselines were captured on the reference machine.

if(NOT DEFINED THRESHOLD_PCT)
    set(THRESHOLD_PCT 25)
endif()
if(NOT BENCH_EXE OR NOT EXISTS "${BENCH_EXE}")
    message(FATAL_ERROR "perf_gate: BENCH_EXE not found: '${BENCH_EXE}'")
endif()
if(NOT BASELINE)
    message(FATAL_ERROR "perf_gate: BASELINE path not provided")
endif()

execute_process(
    COMMAND "${BENCH_EXE}" --gate
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE  _err
    RESULT_VARIABLE _rc
    TIMEOUT 300)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "perf_gate: '${BENCH_EXE} --gate' exited ${_rc}\n${_out}\n${_err}")
endif()

# Pull every "GATE <name> <int>" line out of the bench output.
string(REGEX MATCHALL "GATE[ \t]+[A-Za-z0-9_]+[ \t]+[0-9]+" _measured "${_out}")
if(_measured STREQUAL "")
    message(FATAL_ERROR "perf_gate: no GATE metrics in bench output:\n${_out}")
endif()

# --- capture mode: (re)write the baseline and stop ------------------------
if(UPDATE_BASELINE)
    get_filename_component(_exe_name "${BENCH_EXE}" NAME)
    set(_content "# perf baseline for ${_exe_name}\n# captured by perf_gate.cmake UPDATE_BASELINE=ON — slower-is-worse integer metrics\n")
    foreach(_m IN LISTS _measured)
        string(REGEX MATCH "GATE[ \t]+([A-Za-z0-9_]+)[ \t]+([0-9]+)" _ "${_m}")
        set(_content "${_content}${CMAKE_MATCH_1} ${CMAKE_MATCH_2}\n")
    endforeach()
    file(WRITE "${BASELINE}" "${_content}")
    message(STATUS "perf_gate: wrote baseline ${BASELINE}")
    return()
endif()

if(NOT EXISTS "${BASELINE}")
    message(FATAL_ERROR
        "perf_gate: baseline missing: ${BASELINE}\n"
        "  capture one on this machine with:\n"
        "  cmake -DBENCH_EXE=${BENCH_EXE} -DBASELINE=${BASELINE} -DUPDATE_BASELINE=ON -P perf_gate.cmake")
endif()

# Load baseline "<name> <int>" lines into BASE_<name> variables.
file(STRINGS "${BASELINE}" _base_lines)
foreach(_line IN LISTS _base_lines)
    if(_line MATCHES "^[ \t]*#" OR _line STREQUAL "")
        continue()
    endif()
    if(_line MATCHES "^[ \t]*([A-Za-z0-9_]+)[ \t]+([0-9]+)")
        set("BASE_${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
    endif()
endforeach()

set(_regressions "")
set(_report "")
foreach(_m IN LISTS _measured)
    string(REGEX MATCH "GATE[ \t]+([A-Za-z0-9_]+)[ \t]+([0-9]+)" _ "${_m}")
    set(_name "${CMAKE_MATCH_1}")
    set(_val  "${CMAKE_MATCH_2}")
    if(NOT DEFINED BASE_${_name})
        set(_report "${_report}  ${_name}: measured=${_val}  (no baseline — skipped)\n")
        continue()
    endif()
    set(_base "${BASE_${_name}}")
    math(EXPR _limit "${_base} * (100 + ${THRESHOLD_PCT}) / 100")
    # pct delta vs baseline, integer, for the report line (signed)
    if(_base GREATER 0)
        math(EXPR _delta "(${_val} - ${_base}) * 100 / ${_base}")
    else()
        set(_delta "?")
    endif()
    if(_val GREATER _limit)
        set(_report "${_report}  ${_name}: measured=${_val}  baseline=${_base}  (+${_delta}%)  > limit ${_limit}  REGRESSION\n")
        list(APPEND _regressions "${_name}")
    else()
        set(_report "${_report}  ${_name}: measured=${_val}  baseline=${_base}  (${_delta}%)  ok (limit ${_limit})\n")
    endif()
endforeach()

message(STATUS "perf_gate ${BENCH_EXE} (threshold +${THRESHOLD_PCT}%):\n${_report}")

if(NOT _regressions STREQUAL "")
    list(JOIN _regressions ", " _rj)
    message(FATAL_ERROR "perf_gate: PERFORMANCE REGRESSION in: ${_rj} (see report above)")
endif()
