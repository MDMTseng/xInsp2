//
// test_pack_ledger_misattribution.cpp — single-creator-tag invariants
// (formerly the R1 counted-ledger regression, docs/new_gen/22-plausible-triage.md).
//
// HISTORY. The PackRegistry once kept a counted per-owner ref ledger
// (vector<OwnerRef> per slot). It produced the R1 over-release UAF (a guardless
// release mis-charged owners.back(), popping a live co-owner's bucket, so a
// later sweep freed the pack under the true holder) and the L2 phantom bucket
// (a tagged retain paired with an untagged release stranded a bucket), and
// needed this dedicated misattribution test to pin its clamp arithmetic.
//
// THE MODEL NOW: a single creator tag. seal() stamps {creator, creator_ref_live};
// every retain is an untracked ++rc; release_as clears the tag only when the
// creator drops its OWN ref; the owner sweep (release_all_for) drops AT MOST
// the creator's one seal ref, iff still live. There are no buckets to
// mis-charge and no clamp: the R1 and phantom-bucket classes are structurally
// UNREPRESENTABLE. This test asserts exactly that:
//   §1 sweep never over-releases: a consumer-held pack survives the creator's
//      sweep (the sweep drops at most one ref — the creator's).
//   §2 unattributable releases never over-drop: arbitrary owner tags on
//      release are plain rc decrements; a release on a freed handle no-ops.
//   §3 no double-drop: the creator's own release clears the tag, so its later
//      sweep reclaims nothing (the phantom-tag class is gone).
//   §4 creator-leak reclaim: a creator that seals and forgets to release is
//      swept — the diagnostic the ledger existed for still works.
//   §5 consumer-retain leak is DIAGNOSED, not swept: the consumer's sweep
//      (creator != consumer) leaves the pack alive and readable — no UAF, no
//      vanish; it is a reported live leak (live_frames), reclaimed only at
//      process teardown. Fail toward leak, never toward UAF.
//   §6 handoff untag (the cap/door fix): untag_pack_ref clears the creator tag
//      with rc unchanged, so the creator's sweep spares the handed-off ref.
//
#include <xi/xi_pack_abi.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_abi.h>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdio>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::fprintf(stderr, "\n[section] %s\n", name)

// Seal a pack under `owner`'s guard -> creator = owner, creator_ref_live, rc=1.
static xi_pack_handle seal_as(const xi_pack_v1* pk, xi::ImagePoolOwnerId owner,
                              int64_t payload) {
    xi::ImagePool::OwnerGuard g(owner);
    xi_pack_builder b = pk->builder_new();
    pk->builder_add_i64(b, "payload", payload);
    return pk->builder_seal(b);
}

// Seal a pack WITH NO owner context (main thread, no OwnerGuard) -> no creator
// tag (creator = 0), never touched by any sweep.
static xi_pack_handle seal_untagged(const xi_pack_v1* pk, int64_t payload) {
    xi_pack_builder b = pk->builder_new();
    pk->builder_add_i64(b, "payload", payload);
    return pk->builder_seal(b);
}

int main() {
    std::printf("[test] pack-registry single-creator-tag invariants\n");

    static xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_pack_abi();
    const auto* pk = static_cast<const xi_pack_v1*>(host.get_interface("xi.pack", 1));
    CHECK(pk != nullptr);
    if (!pk) return 2;

    auto& R = xi::PackRegistry::instance();
    const xi::ImagePoolOwnerId A = xi::ImagePool::alloc_owner_id();
    const xi::ImagePoolOwnerId B = xi::ImagePool::alloc_owner_id();
    const size_t baseline = R.live_frames();

    // -----------------------------------------------------------------------
    SECTION("1: the sweep never over-releases — a consumer-held pack survives");
    {
        // Creator A seals (and LEAKS its seal ref); consumer B retains. A's
        // sweep must drop exactly A's one seal ref and no more — the pack
        // survives for B. (Under the old counted ledger this shape — plus a
        // mis-charged bucket — was the R1 UAF; now over-release has no
        // representation: there is only one droppable tracked ref.)
        xi_pack_handle h = seal_as(pk, A, 7);               // rc=1, creator=A live
        CHECK(h != XI_PACK_NULL);
        CHECK(R.owner_refs(A) == 1);
        { xi::ImagePool::OwnerGuard g(B); pk->retain(h); }  // rc=2 (untracked)
        CHECK(R.owner_refs(B) == 0);                        // consumer refs are untracked

        int swept = R.release_all_for(A);                   // A dies, seal ref leaked
        CHECK(swept == 1);                                  // exactly the creator ref
        CHECK(R.pack(h) != nullptr);                        // *** never over-released
        CHECK(R.live_frames() == baseline + 1);

        // B can still safely deref the pack the old ledger could have freed.
        int64_t v = -1; CHECK(pk->get_i64(h, "payload", &v) == 1); CHECK(v == 7);

        // A redundant sweep of A is a no-op (tag already cleared).
        CHECK(R.release_all_for(A) == 0);

        // The true last holder releases -> NOW it frees.
        { xi::ImagePool::OwnerGuard g(B); pk->release(h); }
        CHECK(R.pack(h) == nullptr);
        CHECK(R.live_frames() == baseline);
    }

    // -----------------------------------------------------------------------
    SECTION("2: an unattributable release never over-drops; freed handle no-ops");
    {
        // Sealed off-guard: creator = 0, nothing for any sweep to reclaim.
        xi_pack_handle h = seal_untagged(pk, 11);           // rc=1, no creator tag
        R.retain_as(h, A);                                  // rc=2 (owner arg ignored)
        R.retain_untagged(h);                               // rc=3
        // Releases under arbitrary/wrong owner tags are plain decrements — there
        // is no bucket to mis-charge (the R1 vector is gone).
        R.release_as(h, B);                                 // rc=2
        R.release_as(h, 0);                                 // rc=1
        CHECK(R.pack(h) != nullptr);                        // exactly balanced, alive
        CHECK(R.release_all_for(A) == 0);                   // sweeps see no creator tag
        CHECK(R.release_all_for(B) == 0);
        R.release_as(h, A);                                 // rc=0 -> freed
        CHECK(R.pack(h) == nullptr);
        CHECK(R.live_frames() == baseline);
        // Stray operations on the dead handle: pure no-ops, no double-free.
        R.release_as(h, A);
        R.release_as(h, 0);
        R.retain_as(h, B);
        CHECK(R.release_all_for(A) == 0);
        CHECK(R.live_frames() == baseline);
    }

    // -----------------------------------------------------------------------
    SECTION("3: no double-drop — the creator's own release clears its tag");
    {
        // Creator A releases its OWN ref properly under its guard; consumer B
        // co-holds. A's later sweep must reclaim NOTHING (the phantom-tag /
        // phantom-bucket class is unrepresentable: the tag died with A's
        // release), and B's pack must stay alive.
        xi_pack_handle h = seal_as(pk, A, 21);              // rc=1, creator=A live
        { xi::ImagePool::OwnerGuard g(B); pk->retain(h); }  // rc=2
        { xi::ImagePool::OwnerGuard g(A); pk->release(h); } // rc=1, tag cleared
        CHECK(R.owner_refs(A) == 0);
        CHECK(R.release_all_for(A) == 0);                   // nothing to double-drop
        CHECK(R.pack(h) != nullptr);
        int64_t v = -1; CHECK(pk->get_i64(h, "payload", &v) == 1); CHECK(v == 21);
        { xi::ImagePool::OwnerGuard g(B); pk->release(h); } // B's genuine release
        CHECK(R.pack(h) == nullptr);
        CHECK(R.live_frames() == baseline);
    }

    // -----------------------------------------------------------------------
    SECTION("4: creator-leak reclaim — a forgotten seal ref is still swept");
    {
        // A seals and FORGETS to release; its sweep reclaims the seal ref (the
        // diagnostic the owner tag exists for) and frees the sole-owned pack.
        xi_pack_handle h = seal_as(pk, A, 31);              // rc=1, creator=A live
        CHECK(R.owner_refs(A) == 1);
        int swept = R.release_all_for(A);
        CHECK(swept == 1);
        CHECK(R.pack(h) == nullptr);
        CHECK(R.live_frames() == baseline);
    }

    // -----------------------------------------------------------------------
    SECTION("5: consumer-retain leak is DIAGNOSED, not swept (fail toward leak)");
    {
        // Creator A behaves perfectly; consumer B retains and is destroyed
        // WITHOUT releasing. B's sweep must NOT touch the pack (creator != B):
        // consumer refs are untracked by design, so the leak is DIAGNOSED (a
        // live frame in the table) rather than swept — no UAF, no vanish. It
        // is reclaimed only at process teardown (the registry is intentionally
        // leaked), NOT at B's death and NOT at A's sweep.
        xi_pack_handle h = seal_as(pk, A, 41);              // rc=1, creator=A live
        { xi::ImagePool::OwnerGuard g(B); pk->retain(h); }  // rc=2 (untracked)
        { xi::ImagePool::OwnerGuard g(A); pk->release(h); } // rc=1, tag cleared

        // B dies leaking its ref: the sweep reclaims nothing...
        CHECK(R.release_all_for(B) == 0);
        // ...and the pack neither vanishes nor dangles: still alive + readable.
        CHECK(R.pack(h) != nullptr);
        CHECK(R.live_frames() == baseline + 1);             // the DIAGNOSED leak
        int64_t v = -1; CHECK(pk->get_i64(h, "payload", &v) == 1); CHECK(v == 41);

        // The creator's sweep doesn't reclaim it either (its tag is long gone).
        CHECK(R.release_all_for(A) == 0);
        CHECK(R.pack(h) != nullptr);
        CHECK(R.live_frames() == baseline + 1);             // leaked until teardown

        // (Test hygiene only: drop the leaked ref so the table balances — in
        // production this ref survives to process exit, by design.)
        R.release_as(h, 0);
        CHECK(R.live_frames() == baseline);
    }

    // -----------------------------------------------------------------------
    SECTION("6: handoff untag — the cap/door fix: tag cleared, rc unchanged");
    {
        // A funnel/door output: creator A seals FOR its caller, then hands the
        // seal ref off via untag_pack_ref. The tag clears with rc UNCHANGED, so
        // A's teardown sweep spares the caller's (formerly A's) ref.
        xi_pack_handle h = seal_as(pk, A, 51);              // rc=1, creator=A live
        xi::ImagePool::untag_pack_ref(h, B);                // wrong owner: no-op
        CHECK(R.owner_refs(A) == 1);
        xi::ImagePool::untag_pack_ref(h, A);                // the handoff
        CHECK(R.owner_refs(A) == 0);                        // tag cleared...
        CHECK(R.pack(h) != nullptr);                        // ...rc untouched (still 1)
        CHECK(R.release_all_for(A) == 0);                   // sweep spares the handoff
        CHECK(R.pack(h) != nullptr);
        int64_t v = -1; CHECK(pk->get_i64(h, "payload", &v) == 1); CHECK(v == 51);
        pk->release(h);                                     // the caller's release frees
        CHECK(R.pack(h) == nullptr);
        CHECK(R.live_frames() == baseline);
    }

    CHECK(R.live_frames() == baseline);                     // table balances overall
    if (g_failures == 0) { std::printf("\n[test] pack single-creator-tag: ALL OK\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
