#pragma once
//
// xi_script.hpp — public ABI between xInsp2 backend and dynamically loaded
// user scripts.
//
// A script is a single C++ translation unit compiled into a shared library.
// It must export one symbol:
//
//   extern "C" __declspec(dllexport)
//   void xi_inspect_entry(int frame);
//
// (Linux: just `extern "C" void xi_inspect_entry(int frame);`.)
//
// The backend invokes this function during `cmd: run`. Inside it, the user
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
