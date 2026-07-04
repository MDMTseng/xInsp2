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

#include <cstring>
#include <string>

extern thread_local char  g_run_frame_path_[];   // xi_script_support.hpp
extern thread_local long long g_run_id_;         // xi_script_support.hpp (U3)
extern void* g_use_host_api_;       // xi_script_support.hpp (xi_host_api*)

namespace xi {

inline std::string current_frame_path() {
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
inline long long run_id() {
    return g_run_id_;
}

// [v12 THE CUT — xi::imread was DELETED with the read_image_file host slot.
//  Image decode is the xi.image.decode capability (imgcodec provider): a script
//  that needs a decoded file calls the capability through the xi.cap funnel
//  with a bin "data" request (doc 06 §3.7), or lets a source plugin do the
//  decode and ride the frame in on the pack. No in-tree script used xi::imread
//  at the cut.]

} // namespace xi
