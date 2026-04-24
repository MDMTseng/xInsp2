#pragma once
//
// xi_breakpoint.hpp — poor-man's breakpoint for inspection scripts (S3).
//
// Call xi::breakpoint("label") anywhere inside xi_inspect_entry; the
// call blocks until the WS client sends `cmd: resume`. The host emits a
// text event with { name: "breakpoint", data: { label: "..." } } so the
// client knows where execution has parked.
//
// Usage:
//
//   #include <xi/xi.hpp>
//   #include <xi/xi_breakpoint.hpp>
//
//   void xi_inspect_entry(int frame) {
//       VAR(gray, toGray(cam->grab()));
//       xi::breakpoint("after_gray");           // UI can inspect `gray` here
//       VAR(edges, sobel(gray));
//       xi::breakpoint("after_edges");
//   }
//
// Without a host callback installed (host too old, or breakpoints
// disabled), the call is a no-op — scripts stay portable.
//

#include <string>

// Global-scope pointer, matching the pattern used by xi_use.hpp for
// xi::use callbacks. Defined (as static) inside xi_script_support.hpp
// which is force-included into every user-script DLL.
extern void* g_breakpoint_fn_;

namespace xi {

using BreakpointFn = void (*)(const char* label);

inline void breakpoint(const char* label) {
    auto fn = reinterpret_cast<BreakpointFn>(g_breakpoint_fn_);
    if (fn) fn(label ? label : "");
}

inline void breakpoint(const std::string& label) {
    breakpoint(label.c_str());
}

} // namespace xi
