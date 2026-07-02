//
// fault_plugin.cpp — a controllable-crash xi C-ABI plugin for the post-fault
// quarantine-policy test (test_fault_policy.cpp, adoption-map item 14).
//
// Like golden_plugin it uses RAW C exports (no XI_PLUGIN_IMPL), so a hardware
// fault inside process() propagates OUT to the host's SEH boundary exactly as a
// real crashing plugin would — that is what makes the caught-fault path under
// test genuine rather than simulated. It exposes just enough state to PROVE a
// reinit rebuilt the instance and dropped its in-flight state:
//
//   * a process-wide create SERIAL (bumped per construction) — a reinit makes a
//     NEW instance, so the serial changes;
//   * `value`   — persisted config (get_def/set_def), so a reinit restores it;
//   * `scratch` — in-flight state (exchange "bump"), NOT persisted, so a reinit
//     DROPS it (resets to 0);
//   * `crash_next` — in-flight crash arming (exchange "arm_crash"), also dropped
//     by a reinit, so a rebuilt instance stops crashing.
//
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <xi/xi_abi.h>
#include <xi/xi_record.hpp>   // xi::yyjson_layout_stamp()

namespace {

// Lifetime create serial — proves a reinit constructed a genuinely NEW instance.
std::atomic<int> g_creates{0};

// Sink for the overflow recursion's frame writes so the optimizer can't elide the
// large per-frame buffer (which would defeat the overflow). volatile + a real use.
volatile int g_recurse_sink = 0;

struct FaultInstance {
    const xi_host_api* host = nullptr;
    int  serial     = 0;      // this instance's create serial
    int  value      = 1;      // persisted config (get_def/set_def)
    int  scratch    = 0;      // in-flight state (exchange "bump"); dropped by reinit
    bool crash_next = false;  // in-flight crash arming (exchange "arm_crash"); dropped by reinit
    bool stkoflw_next = false; // arm a deterministic STACK_OVERFLOW inside process()
    bool deep_next    = false; // run a bounded, SAFE deep recursion inside process()
    int  deep_result  = -1;    // its result — proves the stack is healthy post-recovery
};

// Deterministic STACK_OVERFLOW: a large per-frame buffer (16 KB) recursed with no
// reachable base case within the stack's reach, so the guard page is hit in a few
// dozen frames — fast and repeatable. The `depth` bound is an impossible backstop
// (the overflow always fires first) that also keeps this a well-defined, non-infinite
// function. The volatile buffer touches + global sink defeat tail-call / dead-store
// elimination that would otherwise remove the frame.
void recurse_overflow(volatile int depth) {
    volatile char frame[16 * 1024];
    frame[0]                 = (char)depth;
    frame[sizeof(frame) - 1] = (char)(depth + 1);
    if (depth < 1000000) recurse_overflow(depth + 1);
    g_recurse_sink += frame[0] + frame[sizeof(frame) - 1];
}

// Bounded, SAFE deep recursion — proves a thread that survived a caught overflow has
// a healthy stack again. Returns 0+1+...+n. Small frames + depth 512 stay well within
// a worker's stack; volatile keeps every frame real.
int recurse_sum(int n) {
    volatile int local = n;
    if (n <= 0) return 0;
    return local + recurse_sum(n - 1);
}

// Expected checksum for recurse_sum(kDeepDepth), asserted by the test.
constexpr int kDeepDepth   = 512;
constexpr int kDeepExpected = kDeepDepth * (kDeepDepth + 1) / 2;

} // namespace

extern "C" {

__declspec(dllexport) int xi_plugin_abi_version(void) { return XI_ABI_MIN_COMPAT; }
__declspec(dllexport) uint32_t xi_yyjson_abi(void) { return xi::yyjson_layout_stamp(); }

__declspec(dllexport) void* xi_plugin_create(const xi_host_api* host, const char* /*name*/) {
    auto* i = new FaultInstance();
    i->host = host;
    i->serial = ++g_creates;
    return i;
}
__declspec(dllexport) void xi_plugin_destroy(void* p) { delete static_cast<FaultInstance*>(p); }

// Raw export: an armed process() faults with an ACCESS_VIOLATION that propagates
// to the host's SEH boundary (use_process_inline_) — a real caught fault.
__declspec(dllexport) void xi_plugin_process(void* p, const xi_record* /*in*/, xi_record_out* /*out*/) {
    auto* i = static_cast<FaultInstance*>(p);
    if (!i) return;
    if (i->crash_next) {
        volatile int* np = nullptr;
        *np = 42;   // ACCESS_VIOLATION -> seh_exception at the host boundary
    }
    if (i->stkoflw_next) {
        // STACK_OVERFLOW -> seh_exception at the host boundary. Clear the arming FIRST:
        // recurse_overflow never returns, and the write to this heap member survives the
        // SEH unwind, so a re-armed second cycle can run a DIFFERENT mode next time.
        i->stkoflw_next = false;
        recurse_overflow(0);
    }
    if (i->deep_next) {
        i->deep_next   = false;             // one-shot
        i->deep_result = recurse_sum(kDeepDepth);
    }
}

__declspec(dllexport) int xi_plugin_get_def(void* p, char* buf, int cap) {
    auto* i = static_cast<FaultInstance*>(p);
    return std::snprintf(buf, (size_t)cap, "{\"value\":%d}", i ? i->value : 0);
}
__declspec(dllexport) int xi_plugin_set_def(void* p, const char* json) {
    auto* i = static_cast<FaultInstance*>(p);
    if (!i || !json) return 1;
    if (const char* k = std::strstr(json, "\"value\"")) {
        if (const char* c = std::strchr(k, ':')) i->value = std::atoi(c + 1);
    }
    return 0;
}

// exchange: "bump" (++scratch), "arm_crash" (crash_next=true), anything else just
// reports. Always returns the current {serial,value,scratch,armed}.
__declspec(dllexport) int xi_plugin_exchange(void* p, const char* cmd, char* buf, int cap) {
    auto* i = static_cast<FaultInstance*>(p);
    if (!i || !cmd) return 0;
    if (std::strstr(cmd, "arm_crash"))        i->crash_next   = true;
    else if (std::strstr(cmd, "arm_stkoflw")) i->stkoflw_next = true;
    else if (std::strstr(cmd, "deep"))        i->deep_next    = true;
    else if (std::strstr(cmd, "bump"))        ++i->scratch;
    return std::snprintf(buf, (size_t)cap,
        "{\"serial\":%d,\"value\":%d,\"scratch\":%d,\"armed\":%d,\"deep_result\":%d}",
        i->serial, i->value, i->scratch, i->crash_next ? 1 : 0, i->deep_result);
}

// A factory that always FAILS — used to force reinit rebuild failures so the
// escalation-to-refuse path (after kReinitEscalateAfter) can be exercised.
__declspec(dllexport) void* xi_plugin_create_null(const xi_host_api* /*host*/, const char* /*name*/) {
    return nullptr;
}

} // extern "C"
