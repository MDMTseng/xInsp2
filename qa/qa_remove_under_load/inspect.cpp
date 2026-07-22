// qa_remove_under_load — the Finding-5 stress regression: remove_instance under
// continuous dispatch load (docs/new_gen/21-redteam-load-findings.md).
//
// mock_camera (PACK MODE) self-drives a continuous stream. Per trigger this
// script CHAINS the trigger pack into the churned "victim" instance's xi.pack@1
// door (xi::use("victim").process) so the victim's owner-tagged pack refs ride
// the dispatch flow WHILE the driver hammers remove_instance / create_instance
// on it. remove_instance is the lifecycle op the red-team pass found had no
// quiesce guard: without the guard a dispatch worker can release an
// owner-tagged ref into a half-torn instance (ledger mis-attribution UAF). The
// fix wraps it in quiesce_dispatch_for_lifecycle_op_ like every sibling op; this
// script keeps refs in flight so the driver actually exercises that window.
//
// The victim call is best-effort: when the victim is absent (just removed) use()
// returns an empty pack; when present its door may fault on the unmatched input
// — either way the tick verdicts ok. The driver asserts SURVIVAL + consistency,
// not any per-frame value.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;
    auto tp = t.pack();
    if (tp) {
        // Keep the victim's refs in the dispatch flow (result ignored on purpose).
        auto r = xi::use("victim").process(tp);
        (void)r;
    }
    xi::ok(1, "remove_under_load tick");

    xi::ScriptPackBuilder rb;
    rb.add_str("$channel", "rul");
    rb.add_i64("tick", 1);
    if (auto out = rb.seal()) xi::use("expose").push(out);
}
