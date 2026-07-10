#pragma once
//
// xi_io.hpp — host-mediated file I/O + per-run context for inspection
// scripts.
//
// Scripts that need to read a frame from disk (typical AI-driven
// workflow: agent points the backend at a PNG and asks for a result)
// use:
//
//   #include <xi/xi.hpp>      // pulls in xi_io.hpp
//
//   void xi_inspect_entry(int) {
//       auto frame = xi::imread(xi::current_frame_path());
//       VAR(input, frame);
//       ...
//   }
//
// Both pieces are necessary:
//   - `current_frame_path()` reads the value the host sets per
//     `cmd:run` via the `frame_path` arg.
//   - `imread()` decodes PNG / JPEG / BMP / TGA / GIF / PSD / HDR / PIC
//     into an xi::Image. Goes through host_api so the host's
//     stb_image is the single decoder; the script DLL doesn't have to
//     vendor / link it.
//
// These accessors are script-only — they reach into globals defined
// by xi_script_support.hpp (force-included into every script DLL).
// Plugins never include xi.hpp / xi_io.hpp, so the externs below
// don't bite plugin / backend translation units.
//

#include "xi_abi.h"
#include "xi_async.hpp"   // current_inspect_ticket_ref — off-thread guard below
#include "xi_image.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

extern thread_local char  g_run_frame_path_[];   // xi_script_support.hpp
extern thread_local long long g_run_id_;         // xi_script_support.hpp (U3)
extern void* g_use_host_api_;       // xi_script_support.hpp (xi_host_api*)

namespace xi {

namespace detail {

// FAIL-LOUD off-thread guard for the per-run context accessors below.
//
// ROOT CAUSE this diagnoses: g_run_id_ / g_run_frame_path_ are thread_local,
// set by the host on the DISPATCH (inspect) thread only, and xi::async /
// xi::parallel_for do NOT propagate them to their workers. A script that
// stamps `b.add_i64("$seq", xi::run_id())` or opens
// `xi::current_frame_path()` from inside an async/parallel body therefore
// silently gets 0 / "" — collapsing frame correlation ($seq=0 on every frame)
// or failing on an empty path. The trigger read path fails LOUD for the same
// misuse (F4 warn_trigger_off_thread_ in service_sinks.cpp); this brings the
// per-run-context accessors up to the same standard. The REAL fix — marshal
// the per-run context into the workers like the owner / trigger-ctx / cancel
// -ticket scopes already do — is deferred; until then, misuse warns instead
// of silently corrupting data.
//
// DETECTION is relational, mirroring F4 exactly:
//   * xi::current_inspect_ticket_ref() (xi_async.hpp) is a thread_local the
//     host sets NONZERO on the inspect thread (xi_script_inspect_begin, drawn
//     per inspect) and that xi::async / xi::parallel_for DO propagate by value
//     into their workers (cancel-scope Scope / TicketScope).
//   * g_run_id_ is NOT propagated, and 0 is its "no live run on this thread"
//     sentinel (host run ids start at 1 — `++g_eng.run_id` in
//     service_dispatch.cpp — and the host clears to 0 after each inspect).
// So: ticket != 0 && g_run_id_ == 0  ⇒  this thread is a CHILD WORKER of a
// live inspect reading per-run context that never reached it → warn.
//
// Quiet (no warn) cases, preserving existing contracts:
//   * On the inspect thread of a live run: g_run_id_ != 0 → real value.
//   * Outside any inspect (DLL load, unit tests): ticket == 0 → 0/"" per the
//     documented contract.
//   * Older host that never calls xi_script_inspect_begin: ticket stays 0
//     everywhere → detection degrades to off, like F4's unwired-thunk degrade.
// KNOWN false positive (documented, warn-once, non-fatal): a host generation
// that calls xi_script_inspect_begin but not the optional xi_script_set_run_id
// would warn ON the inspect thread; the in-tree host wires both
// (service_inspect.cpp) so this only affects mismatched out-of-tree hosts.
inline bool run_context_missing_off_thread_() {
    return ::g_run_id_ == 0 && ::xi::current_inspect_ticket_ref() != 0;
}

// Once-per-site loud warning (Release-style warn-once, matching the F4 guard's
// NDEBUG branch; the value is still returned — a stamped 0 is now DIAGNOSED
// instead of silent). `warned` lives at the call site so run_id() and
// current_frame_path() each surface once.
inline void warn_run_context_off_thread_(std::atomic<bool>& warned, const char* what) {
    if (warned.exchange(true, std::memory_order_relaxed)) return;
    std::fprintf(stderr,
        "ERROR: [xinsp2] %s called off the inspect thread (inside a xi::async / "
        "xi::parallel_for body) — the per-run context is thread_local on the "
        "inspect thread and is NOT propagated to workers, so this call returns "
        "the empty/0 sentinel, not this run's value. Read it ON the inspect "
        "thread and capture by value into the parallel body (see the "
        "Parallelism section of docs/guides/write-a-script.md).\n", what);
    std::fflush(stderr);
}

} // namespace detail

inline std::string current_frame_path() {
    // Off-thread misuse → warn once, then keep the documented return ("" here):
    // existing on-thread callers and the "no frame_path arg" case are unchanged.
    // (The discriminator is g_run_id_ == 0, NOT an empty path — an empty path is
    // legitimate on-thread whenever cmd:run carried no frame_path arg.)
    if (detail::run_context_missing_off_thread_()) {
        static std::atomic<bool> warned{false};
        detail::warn_run_context_off_thread_(warned, "xi::current_frame_path()");
    }
    return std::string(g_run_frame_path_);
}

// U3 (docs/new_gen/17): the arrival/run id of the inspection this thread is
// computing — assigned by the host at dequeue (frame-arrival order; monotonic
// on the wire under parallelism.result_order:"arrival"), set before the
// inspect runs, cleared after. 0 outside an inspect, and 0 on an older host
// (the xi_script_set_run_id export is optional). This is the host truth the
// Record-era ordered sinks got stamped as `$seq` at flush; on the pack plane
// the PRODUCER stamps it before seal (sealed packs are immutable):
//
//   b.add_i64("$seq", (int64_t)xi::run_id());
//
// Thread discipline mirrors current_frame_path(): read it ON the inspect
// thread and capture by value into xi::async / xi::parallel_for bodies.
// Reading it from a worker body instead is the $seq=0 silent-corruption bug —
// now detected by the fail-loud guard (see detail::run_context_missing_off_thread_
// above): off-thread → warn once, still return 0 (the sentinel, unchanged).
inline long long run_id() {
    if (detail::run_context_missing_off_thread_()) {
        static std::atomic<bool> warned{false};
        detail::warn_run_context_off_thread_(warned, "xi::run_id()");
    }
    return g_run_id_;
}

// [v12 THE CUT — xi::imread was DELETED with the read_image_file host slot.
//  Image decode is the xi.image.decode capability (imgcodec provider): a script
//  that needs a decoded file calls the capability through the xi.cap funnel
//  with a bin "data" request (doc 06 §3.7), or lets a source plugin do the
//  decode and ride the frame in on the pack. No in-tree script used xi::imread
//  at the cut.]

} // namespace xi
