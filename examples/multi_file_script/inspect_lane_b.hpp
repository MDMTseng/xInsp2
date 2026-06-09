#pragma once
//
// Lane B — a second lane in its own file (inspect_* stem so it's associated with
// the script). Same deal as inspect_lane_a.hpp: included into inspect.cpp's TU,
// so all script facilities are available.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <string>

inline void run_lane_b(int frame) {
    VAR(lane_b_frame, frame);
    VAR(lane_b_tag, std::string("beta"));
}
