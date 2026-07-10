#pragma once
//
// xi_parallel.hpp — the blessed parallel-pixel-loop wrapper for inspection
// routines. One call folds in all the boilerplate that a hand-written
// `#pragma omp parallel for` gets wrong:
//
//   xi::parallel_for(rows, [&](int y){ process_row(y); });
//
// What it handles for you (see docs/internals/core_fix_plan.md Problem B + C3):
//
//   * SEH safety — installs the SEH translator on EACH OpenMP worker thread.
//     The translator is per-thread on MSVC; the OpenMP runtime spawns its own
//     pool threads with NONE, so a hardware fault (access violation, divide by
//     zero, …) inside a raw omp region is not converted to a catchable
//     xi::seh_exception and TERMINATES THE WHOLE BACKEND. Here every worker gets
//     one.
//   * Cross-region discipline — a C++ exception must NOT propagate out of a
//     `#pragma omp` region (Invariant 6.2). We catch EVERYTHING inside the loop
//     body — the translated xi::seh_exception AND any ordinary std::exception —
//     record the first one, and rethrow it ON THE INSPECT THREAD after the
//     region joins.
//   * Cooperative cancellation — polls xi::cancellation_requested() and SKIPs
//     the remaining iterations (OpenMP forbids breaking out of a for-region), so
//     a watchdog-cancelled inspect drains the loop quickly. C1 SAFETY: if
//     iterations WERE skipped and the skip was the watchdog's TARGETED cancel,
//     parallel_for throws xi::inspect_cancelled after the region joins (see
//     below) instead of returning normally — so the inspect unwinds rather than
//     computing a verdict over half-processed buffers.
//   * Owner attribution (C3) — reads the inspect-thread image-pool owner ONCE
//     before the region and re-installs it per worker (detail::OwnerScope),
//     exactly as it installs the SEH translator, so parallel-created pool images
//     are attributed instead of anonymous (owner=0).
//
// IMPORTANT: the body runs on worker threads, so it must NOT call
// xi::current_trigger() (the trigger is ambient on the inspect thread only — a
// misuse fails loud, see A1). Snapshot it first and capture by value:
//
//   auto snap = xi::trigger_snapshot();                          // inspect thread
//   xi::parallel_for(rows, [&, snap](int y){ use(snap.image("gray"), y); });
//
// Builds with or without /openmp: without it, parallel_for runs serially with
// identical cancel/catch semantics.
//

#include "xi_async.hpp"   // cancellation_requested, detail::owner_get / OwnerScope
#include "xi_seh.hpp"     // install_seh_translator, seh_exception
#include "xi_crash_dump.hpp"   // xi::crash::reserve_fault_stack — 128 KB dump headroom

#include <atomic>
#include <cstdint>
#include <exception>
#include <utility>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace xi {

// C1 SAFETY: thrown by parallel_for (on the calling/inspect thread, AFTER the
// region joins) when a watchdog-TARGETED cooperative cancel actually skipped
// iterations. Before this, a targeted cancel made parallel_for RETURN NORMALLY
// with the tail of the loop silently undone — the script then computed a verdict
// over half-processed buffers and the host shipped it (a possibly-wrong PASS +
// staged PLC actuation) as if the frame were fully inspected. Throwing unwinds
// the whole inspect instead; the HOST (service_inspect.cpp) catches this exact
// type and routes the run to a no-verdict / no-actuation outcome. Defined here —
// a header BOTH the script DLL and the backend compile — so the catch-by-type
// works across the module boundary, exactly like the std::exception catches the
// host already does around s.inspect(). Deliberately NOT a seh_exception: a
// cancel is not a crash (the host's crash path is unchanged).
struct inspect_cancelled : std::exception {
    const char* what() const noexcept override {
        return "inspect cancelled (watchdog targeted cancel skipped parallel_for iterations)";
    }
};

// C1: the watchdog-targeted-cancel predicate ALONE — the same ticket-vs-cutoff
// test cancellation_requested() applies, WITHOUT the per-task xi::async token.
// Keeping the token out means a Future::cancel() / dropped-Future cooperative
// cancel keeps its old return-normally semantics; only the watchdog cancel
// (armed via xi_script_set_global_cancel → arm_cancel) escalates to the throw.
// NOTE (module boundary): these statics live per module — in the SCRIPT DLL this
// reads the state the host armed through the exported thunk; in the backend it
// reads the backend's own copy (never armed by the watchdog → always false), so
// backend-internal parallel_for users are unaffected.
inline bool watchdog_cancel_targets_this_inspect() {
    if (!cancel_active().load(std::memory_order_relaxed)) return false;
    const uint64_t my = current_inspect_ticket_ref();
    // my == 0 ⇒ no ticket drawn (legacy script / outside any inspect):
    // cancellation_requested() treats that as "in flight" — mirror it here.
    return my == 0 || my < cancel_cutoff().load(std::memory_order_relaxed);
}

// Run body(i) for i in [0, n) across OpenMP worker threads. body must be safe to
// call concurrently for distinct i. Rethrows (on the calling/inspect thread) the
// first fault any iteration raised. n <= 0 is a no-op.
template <class F>
void parallel_for(int n, F&& body) {
    if (n <= 0) return;

    // First fault wins. xi::seh_exception carries only a raw SEH code (no other
    // copyable payload), so we stash that; any other exception is captured as a
    // std::exception_ptr. The atomic flag elects a single writer — exchange(true)
    // returns false for exactly one thread, which then owns the handoff slot.
    std::atomic<bool>     faulted{false};
    std::atomic<unsigned> seh_code{0};
    std::exception_ptr    cpp_eptr;   // written only by the flag-CAS winner

    // C1: did any iteration observe a cooperative cancel and SKIP its work? A
    // loop that ran every iteration before the cancel armed is complete — it
    // must NOT be failed. Only skipped-work + a watchdog-targeted cancel (both
    // checked after the region joins) escalates to the inspect_cancelled throw.
    std::atomic<bool>     cancelled_skip{false};

    // C3: capture the image-pool owner ONCE on the inspect thread, before the
    // region — re-installed per worker inside (0 when the owner thunk is unwired).
    const uint32_t parent_owner = detail::owner_get();

    // F4: capture the trigger-context marker ONCE on the inspect thread — re-installed
    // per worker so a current_trigger() read from a parallel body is caught as an
    // off-thread misuse (the ambient trigger lives on the inspect thread only; the
    // body must capture xi::trigger_snapshot() by value). 0 when the thunk is unwired.
    const uint32_t parent_trigger_ctx = detail::trigger_ctx_get();

    // G3: capture the inspect cancel-ticket + token ONCE on THIS (inspect) thread,
    // before the region. Without re-installing them per worker, OMP workers keep
    // ticket 0, which cancellation_requested() treats as "cancel me" the instant a
    // watchdog cancel is armed — so a watchdog-SPARED fresh frame (ticket >= cutoff)
    // has its worker `omp for` chunk silently skipped yet still returns inspect_ok,
    // a wrong verdict in the ~1s cancel window. Mirrors xi::async's Scope.
    const uint64_t parent_ticket = current_inspect_ticket_ref();
    CancelToken* const parent_cancel_token = current_cancel_token_ref();
#ifdef _OPENMP
    #pragma omp parallel
    {
        xi::crash::reserve_fault_stack();               // 128 KB dump headroom (mirrors lane path)
        xi::install_seh_translator();                   // per-OMP-thread (B1)
        detail::OwnerScope owner_scope(parent_owner);   // per-worker owner (C3)
        detail::TriggerCtxScope trigger_ctx_scope(parent_trigger_ctx);   // per-worker trigger ctx (F4)
        // G3: re-install the parent inspect ticket + cancel token on this worker so
        // it is not mistaken for ticket 0 (= unconditionally cancellable).
        struct TicketScope {
            uint64_t     prev_ticket;
            CancelToken* prev_tok;
            TicketScope(uint64_t t, CancelToken* k)
                : prev_ticket(current_inspect_ticket_ref()),
                  prev_tok(current_cancel_token_ref()) {
                current_inspect_ticket_ref() = t;
                current_cancel_token_ref()   = k;
            }
            ~TicketScope() {
                current_inspect_ticket_ref() = prev_ticket;
                current_cancel_token_ref()   = prev_tok;
            }
        } ticket_scope(parent_ticket, parent_cancel_token);
        #pragma omp for
        for (int i = 0; i < n; ++i) {
            // Drain fast once anyone has faulted; honour a cooperative cancel
            // (C1: record that work was actually skipped — see post-region check).
            if (faulted.load(std::memory_order_relaxed)) continue;
            if (xi::cancellation_requested()) {
                cancelled_skip.store(true, std::memory_order_relaxed);
                continue;
            }
            try {
                body(i);
            } catch (const xi::seh_exception& e) {
                // Translated hardware fault — must be caught INSIDE the region.
                if (!faulted.exchange(true, std::memory_order_relaxed))
                    seh_code.store(e.code, std::memory_order_relaxed);
                // The OpenMP runtime KEEPS this worker thread in its pool and reuses it
                // for the next parallel_for. A STACK_OVERFLOW consumed its guard page;
                // restore it here (on the faulting worker — the rethrow below lands on a
                // DIFFERENT thread) or hard-exit if it can't be restored.
                xi::recover_seh_stack_or_die(e.code, "OpenMP worker");
            } catch (...) {
                // GAP FIX (beyond the B1 sketch): an ordinary std::exception from
                // body(i) must not escape the omp region either (Invariant 6.2).
                // Capture the first one and rethrow on the inspect thread.
                if (!faulted.exchange(true, std::memory_order_relaxed))
                    cpp_eptr = std::current_exception();
            }
        }
        // Implicit omp barrier here flushes the winner's writes before the
        // inspect thread reads faulted/seh_code/cpp_eptr below.
    }
#else
    // No OpenMP — run serially, same cancel + catch semantics so a script built
    // without /openmp behaves identically (just single-threaded).
    for (int i = 0; i < n; ++i) {
        if (faulted.load(std::memory_order_relaxed)) break;
        if (xi::cancellation_requested()) {
            // C1: record that work was actually skipped (post-loop check below).
            cancelled_skip.store(true, std::memory_order_relaxed);
            break;
        }
        try {
            body(i);
        } catch (const xi::seh_exception& e) {
            faulted.store(true, std::memory_order_relaxed);
            seh_code.store(e.code, std::memory_order_relaxed);
            break;
        } catch (...) {
            faulted.store(true, std::memory_order_relaxed);
            cpp_eptr = std::current_exception();
            break;
        }
    }
#endif

    // Surface the first fault on the inspect thread, after the region has joined.
    if (faulted.load(std::memory_order_relaxed)) {
        if (cpp_eptr) std::rethrow_exception(cpp_eptr);
        throw xi::seh_exception(seh_code.load(std::memory_order_relaxed));
    }

    // ---- C1 SAFETY (deliberate WIRE-BEHAVIOR-relevant change) ----------------
    // Iterations were SKIPPED and this inspect is targeted by the watchdog's
    // cooperative cancel ⇒ the output buffers are TRUNCATED. Returning normally
    // here (the old behaviour) let the script compute — and the host ship — a
    // verdict + PLC actuation over a half-processed frame. Throw instead so the
    // inspect unwinds; the host maps xi::inspect_cancelled to a no-verdict /
    // no-actuation outcome (never a crash, never a PASS). The fault rethrow
    // above deliberately wins over this: a crash is the more specific diagnosis.
    // A pure Future-token cancel (no watchdog involvement) keeps the old
    // return-normally semantics — see watchdog_cancel_targets_this_inspect().
    if (cancelled_skip.load(std::memory_order_relaxed) &&
        watchdog_cancel_targets_this_inspect())
        throw inspect_cancelled{};
}

} // namespace xi
