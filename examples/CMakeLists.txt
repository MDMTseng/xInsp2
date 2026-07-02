# examples/ — user-authored inspection scripts + their example plugins.
#
# These are NOT built as part of the backend: the backend JIT-compiles each
# inspect.cpp into a DLL at runtime (cl.exe /LD, force-including
# xi/xi_script_support.hpp — see backend/include/xi/xi_script_compiler.hpp).
#
# The COMPILE GATE that keeps every example script building against the current
# headers — the "exemplars are tests" net — lives in backend/CMakeLists.txt as
# the `examples_gate` OBJECT target + ctest (it force-includes the same support
# header the JIT does). Run it with:
#
#     ctest --test-dir backend/build -C Release -R examples_gate
#
# so there is no separate build step here. (The former M0 `demo_async`
# executable was archived to docs/archive/examples/ — it demonstrated the
# removed VAR value-store and no longer compiles.)

cmake_minimum_required(VERSION 3.16)
project(xinsp2_examples NONE)
