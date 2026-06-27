#pragma once
//
// xi_status.hpp — let an inspection script publish a short "what am I doing
// right now" string. Call xi::status("waiting for trigger") anywhere; the host
// keeps the LATEST string (last-write-wins, sticky) under the key "@script" and
// serves it to the UI via cmd:status + a `status` event, and mirrors it into the
// crash breadcrumb so the last status survives a crash into the report.
//
// This is NOT a VAR (per-inspection value) nor a log line (event stream) — it's
// one sticky status string, overwritten in place. Keep it short and human.
//
// Usage:
//   #include <xi/xi.hpp>
//   #include <xi/xi_status.hpp>
//   void xi_inspect_entry(int frame) {
//       xi::status("inspecting frame " + std::to_string(frame));
//       ...
//       xi::status("idle");
//   }
//
// Without a host callback installed (host too old), the call is a no-op —
// scripts stay portable.
//
#include <string>

// Defined (as static) inside xi_script_support.hpp, which is force-included into
// every user-script DLL (same global-callback-pointer pattern as g_use_*_fn_).
extern void* g_status_fn_;

namespace xi {

using StatusFn = void (*)(const char* text);

inline void status(const char* text) {
    auto fn = reinterpret_cast<StatusFn>(g_status_fn_);
    if (fn) fn(text ? text : "");
}

inline void status(const std::string& text) {
    status(text.c_str());
}

} // namespace xi
