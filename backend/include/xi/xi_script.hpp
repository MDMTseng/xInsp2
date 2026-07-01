#pragma once
//
// xi_script.hpp — public ABI between xInsp2 backend and dynamically loaded
// user scripts.
//
// A script is a single C++ translation unit compiled into a shared library.
// It must export ONE of two entry symbols (the loader resolves them in this
// order of preference):
//
//   Script-entry ABI v2 (A4, PREFERRED — explicit trigger, no ambient seam):
//     extern "C" void xi_inspect_entry_tv(const xi_trigger_view* tv, int frame);
//   authored via the blessed macro in <xi/xi_use.hpp>:
//     XI_INSPECT_ENTRY(t, frame) { ... use t ... }
//   The host FILLS a self-contained xi_trigger_view (image handles + meta doc +
//   host_api) and passes it in; the macro builds a xi::Trigger from it. That
//   Trigger touches no thread_local — it is valid on any thread and safe to
//   capture by value into xi::async / xi::parallel_for. This is the root cure
//   for Problem A (core_fix_plan §1.3 A4): the ambient-trigger hazard is gone
//   because there is no ambient trigger to misread.
//
//   Script-entry ABI v1 (LEGACY — ambient trigger via current_trigger()):
//     extern "C" __declspec(dllexport) void xi_inspect_entry(int frame);
//   (Linux: `extern "C" void xi_inspect_entry(int frame);`.)
//   Still fully supported for back-compat: an existing script that exports only
//   this symbol keeps running unchanged, reading the trigger from the ambient
//   thread_local via xi::current_trigger(). New scripts should prefer v2.
//
// Versioning: the entry contract is a SCRIPT-export ABI, versioned by symbol
// PRESENCE (like the optional trigger-leader / meta / owner callbacks) — a clean
// additive break, independent of the xi_host_api struct ABI (XI_ABI_VERSION,
// currently 11, which this change does NOT touch). The loader (xi_script_loader
// .hpp) accepts a DLL exporting either symbol and prefers _tv; the dispatch
// (service_main.cpp run_one_inspection) builds the view when _tv is present.
//
// The backend invokes the entry during `cmd: run`. Inside it, the user
// calls the `VAR()` macro and `xi::*` helpers — the global registries
// (ValueStore, ParamRegistry, InstanceRegistry) are shared between the
// backend exe and the loaded DLL via the normal static/thread-local
// linkage inside `xi/xi.hpp`.
//
// NOTE on symbol sharing: `xi::ValueStore::current()` and friends are
// inline functions with a function-local `static` / `thread_local`. Each
// module that uses them keeps its own instance of the singleton — which
// is exactly what we want for thread-local state BUT it means the backend
// and the script DLL have DIFFERENT stores if they both touch them. To
// bridge, the backend must call into the DLL (via exported thunks) to
// walk the DLL's ValueStore. See service's `xi_script_entry` discipline
// below: the script's inspect fn pushes into the DLL's TLS store, and
// the backend reads back via a second exported function that returns a
// serialized snapshot.
//

#include <cstdint>
#include <cstddef>

#if defined(_WIN32)
  #define XI_SCRIPT_EXPORT extern "C" __declspec(dllexport)
#else
  #define XI_SCRIPT_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// The minimum set a script must export:
//
//   XI_SCRIPT_EXPORT void xi_inspect_entry(int frame);
//
// Optional hooks:
//
//   XI_SCRIPT_EXPORT int  xi_script_snapshot_vars(char* buf, int buflen);
//       Write a JSON array of {name,kind,value?,gid?,width?,height?,channels?}
//       to buf, return bytes written (or -1 if buflen too small).
//   XI_SCRIPT_EXPORT int  xi_script_dump_image(uint32_t gid, uint8_t* out, int cap,
//                                              int* w, int* h, int* c);
//       Copy the image payload for gid into `out` (up to `cap` bytes),
//       write dimensions into w/h/c, return bytes written.
//   XI_SCRIPT_EXPORT int  xi_script_list_params(char* buf, int buflen);
//       Write a JSON array of param descriptors.
//   XI_SCRIPT_EXPORT int  xi_script_set_param(const char* name, const char* value_json);
//       Apply a new value to a named param. Returns 0 on success.
//
// Scripts don't have to implement the optional hooks by hand — the xInsp2
// support header `xi_script_support.hpp` provides default implementations
// that walk the DLL's own xi::ValueStore / ParamRegistry.
