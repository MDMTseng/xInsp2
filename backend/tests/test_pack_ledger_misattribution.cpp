//
// test_pack_ledger_misattribution.cpp — R1 triage
// (docs/new_gen/22-plausible-triage.md).
//
// The PackRegistry owner-ledger (xi_pack_abi.hpp) carries a stated invariant:
//
//     "A release under the wrong/no owner context is reconciled against the
//      untagged headroom first, then against any ledger bucket, so the ledger
//      can never claim more refs than the slot holds and a SWEEP CAN NEVER
//      OVER-RELEASE."
//
// This probe FALSIFIES the "sweep can never over-release" half in the
// multi-owner / no-untagged-headroom corner:
//
//   1. A sealed pack is co-held by TWO tagged owners (A and B) with NO untagged
//      headroom left (the event ref has drained).
//   2. A GUARDLESS release (ImagePool::current_owner()==0 — e.g. a reentrant
//      provider dropping the pack from its OWN worker thread, off any
//      OwnerGuard) arrives. ledger_release() has no untagged headroom to absorb
//      it (sum(ledger)==rc), so it mis-charges the LAST bucket — decrementing an
//      owner that did NOT release. rc still drops correctly, but the ledger now
//      mis-attributes ownership: it believes the guardless releaser's OTHER
//      bucket is the live one.
//   3. The owner-sweep of the mis-charged-FROM owner (its adapter is torn down)
//      then takes its phantom bucket and drives rc to 0 — freeing the pack while
//      the true co-owner still holds a ref. Over-release / latent UAF.
//
// Threat model fit (well-behaved, heavy load): a reentrant provider contracts
// to be thread-safe and the plane runs its handler off multiple dispatch
// threads (xi_cap_abi.hpp header notes). A provider that retains a shared pack
// under its OwnerGuard (tagged) but releases it from a worker thread it spawned
// (no guard -> owner 0), while a second instance co-holds the same pack and the
// emit event's untagged ref has already been dropped, hits exactly this corner.
// RT5's quiesce removed the remove-instance flavour of the guardless release but
// not the plugin-worker flavour.
//
// This is a DELIBERATELY RED test: it asserts the CORRECT behaviour (a co-owned
// pack survives one owner's sweep) which the current ledger VIOLATES, so the exe
// returns non-zero on today's code. CMake wires it WILL_FAIL so ctest stays
// green; when the ledger is fixed the exe returns 0, ctest flips to FAIL, and
// this test + its WILL_FAIL come off together (the fix's regression).
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

int main() {
    std::printf("[test] pack-ledger owner mis-attribution (R1 triage — RED)\n");

    static xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_pack_abi();
    const auto* pk = static_cast<const xi_pack_v1*>(host.get_interface("xi.pack", 1));
    CHECK(pk != nullptr);
    if (!pk) return 2;

    auto& R = xi::PackRegistry::instance();
    const xi::ImagePoolOwnerId A = 0xA11CE;   // two distinct lib-instance owners
    const xi::ImagePoolOwnerId B = 0xB0B;

    const size_t baseline = R.live_frames();

    // Seal WITH NO owner context (main thread, no OwnerGuard) -> the initial ref
    // is UNTAGGED. rc=1, ledger=[], untagged headroom=1.
    xi_pack_builder bld = pk->builder_new();
    pk->builder_add_i64(bld, "payload", 7);
    xi_pack_handle h = pk->builder_seal(bld);
    CHECK(h != XI_PACK_NULL);

    // Two lib instances co-retain it under their OwnerGuards (tagged refs).
    R.retain_as(h, A);   // rc=2, ledger=[{A,1}], untagged=1
    R.retain_as(h, B);   // rc=3, ledger=[{A,1},{B,1}], untagged=1
    CHECK(R.owner_refs(A) == 1);
    CHECK(R.owner_refs(B) == 1);

    // The emit event's untagged ref drains (framework releaser, owner 0). It is
    // absorbed by the untagged headroom — no bucket touched. rc=2, no headroom.
    R.release_as(h, 0);
    CHECK(R.owner_refs(A) == 1);
    CHECK(R.owner_refs(B) == 1);   // both true holders still ledgered

    // Now owner A drops ITS ref from a spawned worker thread — off any
    // OwnerGuard, so current_owner()==0: a GUARDLESS release. There is no
    // untagged headroom (sum(ledger)==rc==2), so ledger_release mis-charges the
    // LAST bucket (B) instead of A. The ledger now WRONGLY believes A holds and
    // B is gone.
    R.release_as(h, 0);

    // CORRECT post-state: A released, B still holds -> owner_refs(A)==0,
    // owner_refs(B)==1. The buggy ledger reports the mirror image.
    CHECK(R.owner_refs(A) == 0);   // RED: current code leaves A's phantom bucket
    CHECK(R.owner_refs(B) == 1);   // RED: current code popped B's real bucket

    // Owner A's adapter is torn down -> its owner sweep. It must NOT free the
    // pack: B still holds a live ref. The bug: the phantom A bucket is swept,
    // rc -> 0, pack freed out from under B.
    R.release_all_for(A);
    CHECK(R.pack(h) != nullptr);                 // RED: pack over-released here
    CHECK(R.live_frames() == baseline + 1);      // RED: frame vanished

    // Clean up the (correctly) surviving co-owner ref so the table balances when
    // the test is later fixed-and-passing.
    if (R.pack(h) != nullptr) R.release_as(h, B);
    CHECK(R.live_frames() == baseline);

    if (g_failures == 0) { std::printf("\n[test] pack-ledger mis-attribution: ALL OK\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES (expected while R1 is unfixed — WILL_FAIL)\n", g_failures);
    return 1;
}
