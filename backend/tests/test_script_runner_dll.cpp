//
// test_script_runner_dll.cpp — minimal "inspection script" for the
// Phase 3 / 3.5 runner E2E tests.
//
// Hand-rolls just enough of the script ABI so the runner can drive it
// without dragging in the full xi_script_support.hpp / ValueStore
// machinery.
//
// Phase 3 covers reset / inspect / snapshot.
//
// Phase 3.5 adds:
//   xi_script_set_use_callbacks  — host installs use_process fn pointer
//   xi_test_set_input            — driver hands the script an input handle
//   inspect_entry                — calls use_process on that handle and
//                                  records the output handle for snapshot
//

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <xi/xi_abi.h>

static int      g_last_frame    = -1;
static int      g_doubled       = -1;
static uint64_t g_input_handle  = 0;
static uint64_t g_output_handle = 0;

// Set by xi_script_set_use_callbacks. Match the signatures expected by
// xi_script_support's xi::use<>() machinery; the runner installs an
// implementation that proxies over IPC back to the backend.
typedef int (*use_process_cb)(const char*, const char*,
                              const xi_record_image*, int,
                              xi_record_out*);
static use_process_cb g_use_process = nullptr;

extern "C" __declspec(dllexport)
void xi_script_set_use_callbacks(void* process_fn, void* /*exchange*/,
                                 void* /*grab*/, void* /*host*/) {
    g_use_process = (use_process_cb)process_fn;
}

extern "C" __declspec(dllexport)
void xi_test_set_input(uint64_t handle) { g_input_handle = handle; }

extern "C" __declspec(dllexport)
void xi_script_reset() {
    g_last_frame    = -1;
    g_doubled       = -1;
    g_output_handle = 0;
}

extern "C" __declspec(dllexport)
void xi_inspect_entry(int frame) {
    g_last_frame = frame;
    g_doubled    = frame * 2;

    // If the harness handed us an input + a use_process callback,
    // exercise the cross-process round-trip: ask "doubler" to process
    // the input and stash the output handle for snapshot.
    if (g_use_process && g_input_handle) {
        xi_record_image in_img{ "frame", g_input_handle };
        xi_record_out   out_rec{};
        int n = g_use_process("doubler", "{}", &in_img, 1, &out_rec);
        if (n > 0 && out_rec.images && out_rec.image_count > 0) {
            g_output_handle = out_rec.images[0].handle;
        }
    }
}

extern "C" __declspec(dllexport)
int xi_script_snapshot_vars(char* buf, int buflen) {
    char tmp[384];
    // Plain JSON array — driver parses for "value":N and "out_handle":H.
    int n = std::snprintf(tmp, sizeof(tmp),
        "[{\"name\":\"frame\",\"kind\":\"number\",\"value\":%d},"
         "{\"name\":\"frame_doubled\",\"kind\":\"number\",\"value\":%d},"
         "{\"name\":\"out_handle\",\"kind\":\"number\",\"value\":%llu}]",
        g_last_frame, g_doubled, (unsigned long long)g_output_handle);
    if (n < 0) return 0;
    if (buflen < n + 1) return -n;
    std::memcpy(buf, tmp, (size_t)n);
    buf[n] = 0;
    return n;
}
