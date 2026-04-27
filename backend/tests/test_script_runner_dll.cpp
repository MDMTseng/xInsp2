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
typedef int (*use_exchange_cb)(const char*, const char*, char*, int);
typedef xi_image_handle (*use_grab_cb)(const char*, int);
static use_process_cb  g_use_process  = nullptr;
static use_exchange_cb g_use_exchange = nullptr;
static use_grab_cb     g_use_grab     = nullptr;

// Where the inspect_entry() smoke records what it observed via the
// exchange/grab callbacks, for the snapshot to report back.
static char     g_exch_rsp_buf[256] = {0};
static int      g_exch_rc           = 0;
static uint64_t g_grabbed_handle    = 0;

extern "C" __declspec(dllexport)
void xi_script_set_use_callbacks(void* process_fn, void* exchange_fn,
                                 void* grab_fn, void* /*host*/) {
    g_use_process  = (use_process_cb)process_fn;
    g_use_exchange = (use_exchange_cb)exchange_fn;
    g_use_grab     = (use_grab_cb)grab_fn;
}

extern "C" __declspec(dllexport)
void xi_test_set_input(uint64_t handle) { g_input_handle = handle; }

extern "C" __declspec(dllexport)
void xi_script_reset() {
    g_last_frame     = -1;
    g_doubled        = -1;
    g_output_handle  = 0;
    g_exch_rsp_buf[0]= 0;
    g_exch_rc        = 0;
    g_grabbed_handle = 0;
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

    // Phase 3.7: smoke the exchange + grab callbacks if installed.
    if (g_use_exchange) {
        g_exch_rc = g_use_exchange("doubler", "{\"hello\":true}",
                                    g_exch_rsp_buf, (int)sizeof(g_exch_rsp_buf));
    }
    if (g_use_grab) {
        g_grabbed_handle = g_use_grab("camera", 100);
    }
}

extern "C" __declspec(dllexport)
int xi_script_snapshot_vars(char* buf, int buflen) {
    char tmp[768];
    // Plain JSON array — driver parses for "value":N and "out_handle":H
    // and the exchange / grab outcomes.
    // exch_rsp is a JSON-string but we want it as a STRING var in the
    // outer array, so escape any inner quotes by replacing with single
    // quotes (test plugin's reply contains "doubler" → easy parse).
    char esc[256];
    {
        const char* s = g_exch_rsp_buf;
        size_t i = 0;
        for (; s[i] && i < sizeof(esc) - 1; ++i) {
            esc[i] = (s[i] == '"') ? '\'' : s[i];
        }
        esc[i] = 0;
    }
    int n = std::snprintf(tmp, sizeof(tmp),
        "[{\"name\":\"frame\",\"kind\":\"number\",\"value\":%d},"
         "{\"name\":\"frame_doubled\",\"kind\":\"number\",\"value\":%d},"
         "{\"name\":\"out_handle\",\"kind\":\"number\",\"value\":%llu},"
         "{\"name\":\"exch_rc\",\"kind\":\"number\",\"value\":%d},"
         "{\"name\":\"exch_rsp\",\"kind\":\"string\",\"value\":\"%s\"},"
         "{\"name\":\"grabbed_handle\",\"kind\":\"number\",\"value\":%llu}]",
        g_last_frame, g_doubled,
        (unsigned long long)g_output_handle,
        g_exch_rc, esc,
        (unsigned long long)g_grabbed_handle);
    if (n < 0) return 0;
    if (buflen < n + 1) return -n;
    std::memcpy(buf, tmp, (size_t)n);
    buf[n] = 0;
    return n;
}
