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
#include "xi_image.hpp"

#include <cstdio>
#include <cstring>
#include <string>

// A4 explicit per-run context read thunks (host-side run_ctx_run_id_cb /
// run_ctx_frame_path_cb), installed via xi_script_set_run_ctx_callbacks. They
// resolve the per-run context the host installed on this thread — the SAME
// context xi::async / xi::parallel_for / xi::spawn_worker propagate onto their
// workers — so these accessors read the RIGHT run on ANY thread (no ambient TLS).
// Null on an older host that doesn't wire them ⇒ the accessors return the 0/""
// sentinel (portable degradation). See xi_script_support.hpp for the storage.
extern void* g_run_ctx_run_id_fn_;      // long long(*)()
extern void* g_run_ctx_frame_path_fn_;  // const char*(*)()
extern void* g_use_host_api_;       // xi_script_support.hpp (xi_host_api*)

namespace xi {

// The optional cmd:run frame_path for THIS run. Rides the A4 explicit per-run
// context: valid on the inspect thread AND on any xi::async / xi::parallel_for /
// xi::spawn_worker worker (the context is propagated by value onto them). OFF a
// live run — or on a raw std::thread / hand-rolled #pragma omp the framework did
// not spawn — the host thunk FAILS LOUD (abort in Debug, warn-once + "" sentinel
// in Release): a per-run accessor with no run context is a programming error, no
// longer a silent empty string. Empty string is a legitimate value on a run that
// carried no frame_path arg. On an older host that doesn't wire the thunk, returns
// "" (portable degradation).
inline std::string current_frame_path() {
    using Fn = const char* (*)();
    auto fn = reinterpret_cast<Fn>(g_run_ctx_frame_path_fn_);
    if (!fn) return {};
    const char* p = fn();
    return std::string(p ? p : "");
}

// The arrival/run id of the inspection this thread is computing — assigned by the
// host at dequeue (frame-arrival order; monotonic on the wire under
// parallelism.result_order:"arrival"). Rides the A4 explicit per-run context, so
// it is valid on the inspect thread AND on any xi::async / xi::parallel_for /
// xi::spawn_worker worker (the spawn gap is closed — a read from a parallel body
// now returns the RIGHT id, not 0). This is the host truth a pack PRODUCER stamps
// into its `$seq` entry before seal (sealed packs are immutable):
//
//   b.add_i64("$seq", (int64_t)xi::run_id());
//
// OFF a live run (or on a thread the framework did not spawn) the host thunk FAILS
// LOUD (abort in Debug, warn-once + 0 in Release) — the $seq=0 silent-corruption
// footgun is gone. On an older host that doesn't wire the thunk, returns 0.
inline long long run_id() {
    using Fn = long long (*)();
    auto fn = reinterpret_cast<Fn>(g_run_ctx_run_id_fn_);
    return fn ? fn() : 0;
}

// [v12 THE CUT — xi::imread was DELETED with the read_image_file host slot.
//  Image decode is the xi.image.decode capability (imgcodec provider): a script
//  that needs a decoded file calls the capability through the xi.cap funnel
//  with a bin "data" request (doc 06 §3.7), or lets a source plugin do the
//  decode and ride the frame in on the pack. No in-tree script used xi::imread
//  at the cut.]

} // namespace xi
