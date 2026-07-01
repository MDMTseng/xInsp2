//
// reload_probe.cpp — fixture "script" DLL for the hot-reload swap race test
// (test_hot_reload_swap.cpp, §28 Tier-2 GAP). It's a minimal loadable script:
// the only symbol load_script() requires is xi_inspect_entry.
//
// xi_inspect_entry() first verifies a module-static magic word set at load time.
// If a reader ever calls into a module that has been FreeLibrary'd out from under
// it (the exact hazard the module_lifetime shared_ptr defends against), that call
// dereferences an unmapped/reused code+data image: it either access-violates
// outright or reads a wrong magic and aborts — either way the race test fails
// loudly instead of corrupting silently.
//
// Raw C exports + DLL-global state, like race_probe/stage_probe: the test drives
// load/unload directly and binds a process-global counter each module writes to.
//
#include <atomic>
#include <cstdlib>

// Module-static, initialized when THIS image is mapped. Distinct per loaded copy.
static volatile unsigned g_magic = 0xABCDABCDu;
static std::atomic<long long>* g_sink = nullptr;   // process-global counter (bound by the test)

extern "C" {

// The one mandatory export (load_script fails without it).
__declspec(dllexport) void xi_inspect_entry(int frame) {
    // Touch the module's own code+data. A call into a freed image faults here;
    // a reused page yields a wrong magic → abort. Either is a caught race.
    if (g_magic != 0xABCDABCDu) std::abort();
    // A little work so the call genuinely executes module code for a while.
    volatile unsigned acc = g_magic ^ (unsigned)frame;
    for (int i = 0; i < 32; ++i) acc = acc * 1664525u + 1013904223u;
    (void)acc;
    if (g_sink) g_sink->fetch_add(1, std::memory_order_relaxed);
}

// Test hook: bind the shared success counter into THIS module. Called right after
// each load so the freshly-mapped image writes to the same process-global counter.
__declspec(dllexport) void reload_probe_bind(void* sink) {
    g_sink = static_cast<std::atomic<long long>*>(sink);
}

__declspec(dllexport) unsigned reload_probe_magic() { return g_magic; }

} // extern "C"
