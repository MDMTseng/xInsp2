//
// test_pack_ledger_misattribution.cpp — R1 regression
// (docs/new_gen/22-plausible-triage.md).
//
// The PackRegistry owner-ledger (xi_pack_abi.hpp) USED to over-release a co-held
// pack: a GUARDLESS release (ImagePool::current_owner()==0 — e.g. a reentrant
// provider dropping a pack from its OWN worker thread, off any OwnerGuard) with
// no untagged headroom mis-charged owners.back() — popping a LIVE co-owner's
// bucket. A later owner-sweep of the mis-charged-FROM owner then drove rc to 0
// and freed the pack out from under the true holder — a UAF.
//
// The fix (R1): an unattributable release never touches a bucket, and the sweep
// reclaims only refs NOT attributable to a surviving owner and never frees while
// another owner's bucket is non-empty — so a co-owned pack frees ONLY when its
// true last holder releases (rc->0). This test pins that invariant with the
// deterministic multi-owner / off-guard repro plus adversarial self-checks:
//   §1 over-release: the exact R1 sequence — the pack must SURVIVE the sweep.
//   §2 double-free:  a redundant sweep + a release on the freed handle are no-ops.
//   §3 leak (defer): the deferred free still happens on the true last release.
//   §4 leak (reclaim): a genuinely forgotten single-owner ref is still reclaimed.
//
// On the pre-fix ledger §1 FAILS (the pack is freed while a co-owner holds); on
// the fixed ledger all sections pass. Normal PASSING ctest target.
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

// Seal a pack WITH NO owner context (main thread, no OwnerGuard) -> the initial
// ref is UNTAGGED (rc=1, ledger empty, one unit of untagged headroom).
static xi_pack_handle seal_untagged(const xi_pack_v1* pk, int64_t payload) {
    xi_pack_builder b = pk->builder_new();
    pk->builder_add_i64(b, "payload", payload);
    return pk->builder_seal(b);
}

int main() {
    std::printf("[test] pack-ledger owner mis-attribution (R1 regression)\n");

    static xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_pack_abi();
    const auto* pk = static_cast<const xi_pack_v1*>(host.get_interface("xi.pack", 1));
    CHECK(pk != nullptr);
    if (!pk) return 2;

    auto& R = xi::PackRegistry::instance();
    const xi::ImagePoolOwnerId A = 0xA11CE;   // two distinct lib-instance owners
    const xi::ImagePoolOwnerId B = 0xB0B;
    const size_t baseline = R.live_frames();

    // -----------------------------------------------------------------------
    SECTION("1: over-release — a co-owned pack survives one owner's sweep");
    {
        xi_pack_handle h = seal_untagged(pk, 7);            // rc=1, untagged=1
        CHECK(h != XI_PACK_NULL);
        R.retain_as(h, A);                                  // rc=2, [{A,1}]
        R.retain_as(h, B);                                  // rc=3, [{A,1},{B,1}]
        R.release_as(h, 0);   // emit event's untagged ref drains (absorbed)  rc=2

        // Owner A drops ITS ref from a spawned worker thread — off any
        // OwnerGuard, so current_owner()==0: a GUARDLESS release with NO untagged
        // headroom. The fix must NOT mis-charge B's bucket.
        R.release_as(h, 0);                                 // rc=1 (B still holds)
        CHECK(R.pack(h) != nullptr);                        // pack alive
        CHECK(R.live_frames() == baseline + 1);
        CHECK(R.owner_refs(B) >= 1);                        // B's bucket preserved

        // A's adapter is torn down -> its owner sweep. It must NOT free the pack:
        // B still holds a live ref. (Pre-fix: freed here -> UAF.)
        int swept = R.release_all_for(A);
        CHECK(R.pack(h) != nullptr);                        // *** the R1 assertion
        CHECK(R.live_frames() == baseline + 1);             // not over-released
        (void)swept;

        // B can still safely deref the pack the sweep would have freed.
        int64_t v = -1; CHECK(pk->get_i64(h, "payload", &v) == 1); CHECK(v == 7);

        // The true last holder releases -> NOW it frees (deferred free happens).
        R.release_as(h, B);
        CHECK(R.pack(h) == nullptr);
        CHECK(R.live_frames() == baseline);
    }

    // -----------------------------------------------------------------------
    SECTION("2: double-free — redundant sweep + release on a freed handle no-op");
    {
        xi_pack_handle h = seal_untagged(pk, 11);
        R.retain_as(h, A);                                  // rc=2, [{A,1}]
        R.release_as(h, 0);                                 // drop untagged  rc=1, [{A,1}]
        int s1 = R.release_all_for(A);                      // sole owner -> reclaim+free
        CHECK(s1 == 1);
        CHECK(R.pack(h) == nullptr);
        CHECK(R.live_frames() == baseline);
        // Redundant sweep + a stray release on the now-dead handle: pure no-ops.
        CHECK(R.release_all_for(A) == 0);
        R.release_as(h, A);
        R.release_as(h, 0);
        CHECK(R.live_frames() == baseline);
    }

    // -----------------------------------------------------------------------
    SECTION("3: leak/defer — a deferred free still happens on the last release");
    {
        // A and B co-hold; A is swept while B genuinely holds (deferred), then B
        // releases normally -> must reach 0 (no leak).
        xi_pack_handle h = seal_untagged(pk, 21);
        R.retain_as(h, A);
        R.retain_as(h, B);
        R.release_as(h, 0);                                 // drop untagged
        R.release_all_for(A);                               // A gone; B holds -> deferred
        CHECK(R.pack(h) != nullptr);
        CHECK(R.live_frames() == baseline + 1);
        R.release_as(h, B);                                 // B's genuine release
        CHECK(R.pack(h) == nullptr);
        CHECK(R.live_frames() == baseline);                 // count reaches 0
    }

    // -----------------------------------------------------------------------
    SECTION("4: leak/reclaim — a forgotten single-owner ref is still reclaimed");
    {
        // A retains and FORGETS to release; its sweep must reclaim the ref (the
        // whole point of the owner ledger) — the fix must not leak it.
        xi_pack_handle h = seal_untagged(pk, 31);
        R.retain_as(h, A);                                  // rc=2, [{A,1}]
        R.release_as(h, 0);                                 // drop untagged  rc=1, [{A,1}]
        CHECK(R.owner_refs(A) == 1);
        int swept = R.release_all_for(A);                   // reclaim the forgotten ref
        CHECK(swept == 1);
        CHECK(R.pack(h) == nullptr);
        CHECK(R.live_frames() == baseline);
    }

    CHECK(R.live_frames() == baseline);                     // table balances overall
    if (g_failures == 0) { std::printf("\n[test] pack-ledger mis-attribution: ALL OK\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
