// qa_semaphore_queue — the inspect half of "custom admission + core envelope".
//
// Per frame (driven by sem_src's semaphore-paced emits into the max_parallel:4,
// result_order:"arrival" lane "q"):
//
//   1. do a little DETERMINISTIC (~20ms fixed) compute so N-way parallelism
//      actually overlaps — the source fills all N permits, so max in-flight
//      reaches N (proving it IS parallel, not serialized);
//   2. stamp a per-frame $seq = xi::run_id() (the host arrival id) and emit the
//      result to the REAL downstream sink (expose — generic + unchanged). The
//      staged push flushes in frame order under result_order:"arrival": that is
//      the ENVELOPE, owned by the core;
//   3. as the LAST step, release the admission permit — drive the ack door
//      INLINE on THIS worker thread (xi::use("sem_ack").process()), so
//      sem_ack.process() fires g_sem.release() at compute-completion
//      ("一個 thread 算完 release"). sem_ack is not a declared sink, so process()
//      runs synchronously here.
//
// The punchline: the plugin owns ADMISSION (a custom N-way semaphore queue) and
// the core owns the ENVELOPE (ordered emit) — and they COEXIST. NO CORE CHANGES.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;             // skip any non-trigger tick

    const long long seq = (long long)xi::run_id();   // $seq: host arrival id

    // 1. deterministic compute so the N lanes overlap
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // 2. ENVELOPE: ordered result to the real (generic, unchanged) sink.
    {
        xi::ScriptPackBuilder rb;
        rb.add_str("$channel", "sem");
        rb.add_i64("$seq", seq);            // ordering entry (frame order on flush)
        rb.add_i64("seq", seq);
        xi::use("expose").push(rb.seal());
    }

    // (emit the verdict — the run_result stream is the core's ordered envelope
    //  the driver checks for strict $seq/run_id order under arrival ordering)
    char msg[64];
    std::snprintf(msg, sizeof msg, "sem seq=%lld", seq);
    xi::ok(1, msg);

    // 3. ADMISSION RELEASE (last step): drive the ack door inline → release one
    //    semaphore permit at compute-completion.
    {
        xi::ScriptPackBuilder ab;
        ab.add_i64("$seq", seq);
        xi::use("sem_ack").process(ab.seal());
    }
}
