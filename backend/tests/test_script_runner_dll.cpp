//
// test_script_runner_dll.cpp — minimal "inspection script" for the
// Phase 3 runner E2E test.
//
// No xi_script_support.hpp dependency — we hand-roll just enough of
// the script ABI (xi_inspect_entry + xi_script_reset +
// xi_script_snapshot_vars) so the runner can drive it without dragging
// in the full ValueStore machinery.
//
// On inspect(frame): records `frame` and `frame * 2`. snapshot reports
// both as scalar number Vars; the test driver parses the JSON and
// verifies the doubling.
//

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

static int g_last_frame    = -1;
static int g_doubled       = -1;

extern "C" __declspec(dllexport)
void xi_script_reset() {
    g_last_frame = -1;
    g_doubled    = -1;
}

extern "C" __declspec(dllexport)
void xi_inspect_entry(int frame) {
    g_last_frame = frame;
    g_doubled    = frame * 2;
}

extern "C" __declspec(dllexport)
int xi_script_snapshot_vars(char* buf, int buflen) {
    char tmp[256];
    int n = std::snprintf(tmp, sizeof(tmp),
        "[{\"name\":\"frame\",\"kind\":\"number\",\"value\":%d},"
         "{\"name\":\"frame_doubled\",\"kind\":\"number\",\"value\":%d}]",
        g_last_frame, g_doubled);
    if (n < 0) return 0;
    if (buflen < n + 1) return -n;
    std::memcpy(buf, tmp, (size_t)n);
    buf[n] = 0;
    return n;
}
