//
// race_probe.cpp — a deliberately UNPROTECTED fixture plugin for the
// set_def-vs-process race test (test_set_def_race.cpp).
//
// "config" is two halves a + b that set_def writes NON-ATOMICALLY (a, widen the
// window, then b). A coherent read must see a == b. process() reads both halves
// and counts every time it catches a != b — i.e. a torn config read. The plugin
// adds ZERO protection of its own, so any torn read means the *host* let set_def
// run concurrently with process().
//
#include <xi/xi_abi.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct Probe {
    volatile int          a = 0;
    volatile int          b = 0;
    std::atomic<long long> tears{0};      // process() saw a != b
    std::atomic<long long> proc_calls{0};
    std::atomic<long long> set_calls{0};
};

extern "C" {

__declspec(dllexport) void* xi_plugin_create(const xi_host_api*, const char*) {
    return new Probe();
}
__declspec(dllexport) void xi_plugin_destroy(void* p) {
    delete static_cast<Probe*>(p);
}

__declspec(dllexport) void xi_plugin_process(void* p, const xi_record*, xi_record_out* out) {
    Probe* s = static_cast<Probe*>(p);
    int x = s->a;            // read the two halves of "config"
    int y = s->b;
    if (x != y) s->tears.fetch_add(1, std::memory_order_relaxed);
    s->proc_calls.fetch_add(1, std::memory_order_relaxed);
    if (out) out->image_count = 0;
}

__declspec(dllexport) int xi_plugin_set_def(void* p, const char* json) {
    Probe* s = static_cast<Probe*>(p);
    const char* c = json ? std::strchr(json, ':') : nullptr;
    int t = c ? std::atoi(c + 1) : 0;
    s->a = t;                                       // non-atomic two-phase write
    for (volatile int i = 0; i < 4000; ++i) {}      // widen the inconsistency window
    s->b = t;                                       // a == b must hold for a coherent read
    s->set_calls.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

__declspec(dllexport) int xi_plugin_get_def(void* p, char* buf, int cap) {
    Probe* s = static_cast<Probe*>(p);
    return std::snprintf(buf, (size_t)cap,
        "{\"tears\":%lld,\"proc\":%lld,\"set\":%lld}",
        (long long)s->tears.load(), (long long)s->proc_calls.load(),
        (long long)s->set_calls.load());
}

__declspec(dllexport) int xi_plugin_exchange(void*, const char*, char* rsp, int cap) {
    if (cap > 2) { rsp[0] = '{'; rsp[1] = '}'; rsp[2] = 0; return 2; }
    return 0;
}

} // extern "C"
