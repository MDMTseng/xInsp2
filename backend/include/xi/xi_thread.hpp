#pragma once
//
// xi_thread.hpp — std::thread wrapper that installs SEH translation and
// catches stray exceptions at thread entry.
//
//     auto t = xi::spawn_worker("camera-poll", [&]{ run_poll_loop(); });
//
// Equivalent to std::thread except the body is wrapped:
//   1. _set_se_translator(seh_translator) — SEH becomes seh_exception
//   2. try/catch around the body — logs and swallows so the process
//      doesn't terminate when a worker thread throws
//
// Plugin authors should prefer this over raw std::thread for any thread
// they spawn from inside the plugin DLL: a stray null-deref on a worker
// thread without an installed translator brings down the whole backend.
//
// `xi::async` (xi_async.hpp) applies the same wrapping for std::async tasks.
//

#include "xi_seh.hpp"
#include "xi_crash_dump.hpp"   // xi::crash::reserve_fault_stack — 128 KB dump headroom
#include "xi_async.hpp"        // xi::detail::owner_get / OwnerScope

#include <cstdio>
#include <exception>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

namespace xi {

template <class F, class... Args>
std::thread spawn_worker(std::string name, F&& f, Args&&... args) {
    // Capture the image-pool owner active on the SPAWNING thread (mirrors
    // xi::async's owner propagation) so pool images the worker creates are
    // attributed to the owning instance and swept on its destroy, instead of
    // owner 0 (anonymous → never swept: a slow leak on hot-reload). No-op /
    // zero when the host hasn't wired the owner thunks.
    uint32_t parent_owner = xi::detail::owner_get();

    // A4: capture the spawning thread's per-run context so xi::run_id() /
    // xi::current_frame_path() / xi::result() called from the worker resolve to the
    // run — closing the spawn gap that used to leave spawn_worker workers
    // context-less. Unlike xi::async / xi::parallel_for (which join before returning,
    // so a POINTER into the spawning frame is safe), spawn_worker is fire-and-forget
    // and may OUTLIVE the inspect — so it takes a worker-OWNED heap SNAPSHOT here (on
    // the spawning thread, while the context is live) and frees it on exit. run_id +
    // frame_path are copied by value; the verdict slot is dropped (a detached worker
    // routes no verdict). Structurally UAF-free — no "don't read after the inspect"
    // convention. Null when off a run / unwired ⇒ the worker just has no context.
    void* run_ctx_snap = xi::detail::run_ctx_snapshot();

    auto closure =
        [name = std::move(name),
         fn   = std::forward<F>(f),
         tup  = std::make_tuple(std::forward<Args>(args)...),
         parent_owner,
         run_ctx_snap]() mutable {
            // Reserve fault-stack headroom FIRST (mirrors xi_async / lane
            // workers) so the crash filter can still write a minidump after
            // a STACK_OVERFLOW on this worker; no-op on non-Windows.
            xi::crash::reserve_fault_stack();
            xi::install_seh_translator();
            // Attribute worker-created pool images to the spawning owner for
            // the duration of the body.
            xi::detail::OwnerScope owner_scope(parent_owner);
            // A4: install the worker-OWNED per-run snapshot on this worker (closes
            // the spawn gap — was OwnerScope-only before). Frees it on exit; safe
            // even if this worker outlives the spawning inspect (no dangling ptr).
            xi::detail::WorkerRunCtxScope run_ctx_scope(run_ctx_snap);
            try {
                std::apply(std::move(fn), std::move(tup));
            } catch (const xi::seh_exception& e) {
                std::fprintf(stderr,
                    "[xinsp2] worker '%s' crashed: 0x%08X (%s)\n",
                    name.c_str(), e.code, e.what());
            } catch (const std::exception& e) {
                std::fprintf(stderr,
                    "[xinsp2] worker '%s' threw: %s\n",
                    name.c_str(), e.what());
            } catch (...) {
                std::fprintf(stderr,
                    "[xinsp2] worker '%s' threw unknown exception\n",
                    name.c_str());
            }
        };
    return std::thread(std::move(closure));
}

} // namespace xi
