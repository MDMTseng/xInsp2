#pragma once
//
// xi_result.hpp — let an inspection script publish the ONE per-run result: a
// signed status code + a human message. Exactly one Result per run (last write
// wins within a run); the host emits it as a dedicated `run_result` event.
//
// Code convention (see docs/design/run-result.md):
//   > 0            OK class   — ok1, ok2, …          (xi::ok(n))
//   = 0            NA / none  — no verdict applicable (the default if unset)
//   -1 … -989999   NG class   — ng1, ng2, …          (xi::ng(n))
//   <= -990000     RESERVED for framework system-fails (dropped / crashed / …);
//                  a user code in this band is NOT silently accepted — the host
//                  records it as NA (0) and emits a warning naming the offending
//                  code, so the framework enum stays the framework's and the
//                  mistake is visible (not hidden behind a fake ng1).
//
// This is NOT a VAR (a run emits many debug/inspection values) — it's the single
// verdict record MES / PLC / the HMI verdict-yield cards consume.
//
// Usage:
//   #include <xi/xi.hpp>
//   #include <xi/xi_result.hpp>
//   void xi_inspect_entry(int frame) {
//       if (defect) xi::ng(2, "edge chip > 0.3mm");   // code -2
//       else        xi::ok(1, "clean");               // code +1
//   }
//
// Without a host callback installed (host too old) the call is a no-op — scripts
// stay portable.
//
#include <string>

// Defined (as static) inside xi_script_support.hpp, force-included into every
// user-script DLL. Same pattern as g_status_fn_.
extern void* g_result_fn_;

namespace xi {

// Lowest user-usable code; anything <= this is the framework system-fail band.
constexpr int kResultSystemBand = -990000;

using ResultFn = void (*)(int code, const char* msg);

inline void result(int code, const char* msg) {
    auto fn = reinterpret_cast<ResultFn>(g_result_fn_);
    if (!fn) return;
    // Pass the raw code through — the host is the trust boundary. It rejects the
    // reserved band (<= kResultSystemBand) with a visible warning rather than a
    // silent clamp, so a mistake isn't hidden behind a fake verdict.
    fn(code, msg ? msg : "");
}

inline void result(int code, const std::string& msg) { result(code, msg.c_str()); }
inline void result(int code) { result(code, ""); }

// Sugar: ok(n) -> +n (n>=1), ng(n) -> -n. n is clamped to >=1 so ok(0)/ng(0)
// don't silently mean NA.
inline void ok(int n = 1, const std::string& msg = "") { result(n < 1 ? 1 : n, msg); }
inline void ng(int n = 1, const std::string& msg = "") { result(n < 1 ? -1 : -n, msg); }

} // namespace xi
