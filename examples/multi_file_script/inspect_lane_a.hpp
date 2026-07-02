#pragma once
//
// Lane A — its own file, #included into the one script TU by inspect.cpp.
// Named with the script's stem (inspect_*) so the extension associates it with
// the script and recompiles on save. xi::use() works here because this compiles
// as part of inspect.cpp's translation unit, NOT as a separate object.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <string>

inline void run_lane_a(int frame) {
    // Surface this lane's values through the `expose` plugin (channel "lane_a").
    xi::use("expose").process(xi::Record()
        .set("$channel", "lane_a")
        .set("lane_a_frame", frame)
        .set("lane_a_tag", std::string("alpha")));

    // Link-proof: xi::use() resolves from inside an #included header (never
    // executed — there's no "unused_a" instance — but it COMPILES + LINKS,
    // which is the whole reason scripts split via headers, not extra .cpp TUs).
    if (frame < 0) { auto& p = xi::use("unused_a"); (void)p; }
}
