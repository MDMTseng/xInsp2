//
// user_script_example.cpp — minimal xInsp2 user script.
//
// Compiled at runtime by `cmd: compile_and_load`, loaded by the service,
// called via `xi_inspect_entry`. The xi_script_support.hpp thunks are
// force-included by the compiler driver, so this file is just inspection
// logic + xi primitives.
//
// Build is driven by the backend; to compile manually for a sanity check:
//
//   cl /std:c++20 /LD /EHsc /MD /O2 /utf-8 /I../backend/include \
//      /FIxi/xi_script_support.hpp user_script_example.cpp
//

#include <xi/xi.hpp>

static xi::Param<int>    user_amp {"user_amp",  5,  {1, 100}};
static xi::Param<double> user_bias{"user_bias", 1.5, {0.0, 10.0}};

static int user_double(int x) { return x * 2; }
ASYNC_WRAP(user_double)

extern "C" __declspec(dllexport)
void xi_inspect_entry(int frame) {
    VAR(raw,     frame);
    VAR(scaled,  raw * static_cast<int>(user_amp));

    auto p1 = async_user_double(scaled);
    VAR(dbl,    int(p1));

    VAR(biased, dbl + static_cast<double>(user_bias));
    VAR(tag,    std::string("user_script_v1"));
    VAR(alive,  true);
}
