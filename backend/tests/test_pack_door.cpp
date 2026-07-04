//
// test_pack_door.cpp — the host side of the xi.pack@1 carved data-plane door
// (polaris2 wave-2, docs/new_gen/08 Wave 2, steps 1-2).
//
// Covers:
//   1. Door probe — host->get_interface("xi.pack", 1) resolves the data plane
//      once installed; a wrong version / absent id is NULL (the same discipline
//      as the other carved interfaces).
//   2. Build/read round-trip through the OPAQUE-HANDLE C accessors — every entry
//      type (i64/f64/str/bin/image/mp), the generic count()/key_at()/tag_at()
//      walk, and fail-closed getters (absent key / wrong tag -> 0).
//   3. Refcount lifecycle — retain/release, sealed-pack immutability, and the
//      pooled-handle balance verified against ImagePool's own live count (a
//      pack's image entry mints exactly one pool handle, freed on last release).
//   4. Dispatch — TriggerBus::emit_pack carries a Pack (the sole dispatch
//      currency after THE CUT; the image + metadata ride inside the pack).
//      The pack + its pooled image refs balance to baseline once dropped.
//
// The plugin-side door (blob_analysis's pack-in/pack-out) + the real end-to-end
// mock_camera->blob_analysis flow are exercised in the PLUGIN test
// (plugins/.../pack_pilot_test) against the actually-built DLLs.
//
#include <xi/xi_pack_abi.hpp>    // PackRegistry, pack_v1_iface, install_pack_abi
#include <xi/xi_image_pool.hpp>   // ImagePool::make_host_api / cumulative().live_now
#include <xi/xi_trigger_bus.hpp>  // TriggerBus (pack-only dispatch)
#include <xi/xi_mp.hpp>           // decode the nested-msgpack round-trip
#include <xi/xi_use.hpp>          // xi::Trigger / xi_trigger_view (H1 has_source)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// xi_use.hpp's Trigger fallback branches (the ambient thread_local path) name
// these script-side host thunks, which normally live in xi_script_support.hpp
// (force-included into a script DLL, not the host). The VIEW-constructed Trigger
// this test builds never takes those branches, but the linker still needs the
// symbols. Null host-side definitions satisfy it; the data_ path never reads them.
void* g_trigger_info_fn_    = nullptr;
void* g_trigger_sources_fn_ = nullptr;
void* g_trigger_leader_fn_  = nullptr;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

static int pool_live() { return xi::ImagePool::instance().cumulative().live_now; }

// ---------------------------------------------------------------------------
// (1) Door probe.
// ---------------------------------------------------------------------------
static void test_door_probe() {
    SECTION("get_interface(\"xi.pack\", 1) resolves the data plane; wrong version NULL");
    xi::install_pack_abi();
    xi_host_api host = xi::ImagePool::make_host_api();
    CHECK(host.get_interface != nullptr);

    const void* v1 = host.get_interface("xi.pack", 1);
    CHECK(v1 != nullptr);
    CHECK(v1 == xi::pack_v1_iface());               // the process-stable singleton
    CHECK(host.get_interface("xi.pack", 2) == nullptr);  // only @1 published
    CHECK(host.get_interface("xi.pack", 0) == nullptr);
    CHECK(host.get_interface("xi.packX", 1) == nullptr);

    // A pack-capable plugin caches this pointer once (Plugin::pack_iface()).
    const auto* fi = static_cast<const xi_pack_v1*>(v1);
    CHECK(fi->builder_new && fi->builder_seal && fi->get_i64 && fi->emit_pack);
}

// ---------------------------------------------------------------------------
// (2) Build/read round-trip through the opaque-handle accessors.
// ---------------------------------------------------------------------------
static void test_build_read_roundtrip() {
    SECTION("build -> seal -> read every entry type through the C accessors");
    xi::install_pack_abi();
    const xi_pack_v1* fi = xi::pack_v1_iface();

    int base = pool_live();

    // A 4x4 single-channel image with a couple of bright pixels.
    std::vector<uint8_t> gray(16, 0);
    gray[5] = 200; gray[6] = 210;

    // A little nested msgpack payload (an array of one {x,y} map).
    xi::mp::Writer mw;
    mw.array(1); mw.map(2); mw.key("x"); mw.int_(3); mw.key("y"); mw.int_(4);

    xi_pack_builder b = fi->builder_new();
    CHECK(b != XI_PACK_BUILDER_NULL);
    fi->builder_add_i64(b, "threshold", 128);
    fi->builder_add_f64(b, "mean_area", 42.5);
    CHECK(fi->builder_add_bool && fi->get_bool);   // additive v1 tail present
    fi->builder_add_bool(b, "pass", 1);
    fi->builder_add_str(b, "label", "part-A", 6);
    const uint8_t blob[3] = {1, 2, 3};
    fi->builder_add_bin(b, "raw", blob, 3);
    fi->builder_add_image(b, "gray", 4, 4, 1, gray.data());
    fi->builder_add_mp(b, "pts", mw.bytes().data(), (int32_t)mw.bytes().size());

    CHECK(pool_live() == base + 1);   // the image entry minted one pool handle

    xi_pack_handle f = fi->builder_seal(b);
    CHECK(f != XI_PACK_NULL);

    // Scalars.
    int64_t i = 0; CHECK(fi->get_i64(f, "threshold", &i) == 1 && i == 128);
    double  d = 0; CHECK(fi->get_f64(f, "mean_area", &d) == 1 && d == 42.5);
    int32_t bv = 0; CHECK(fi->get_bool(f, "pass", &bv) == 1 && bv == 1);
    CHECK(fi->tag_of(f, "pass") == XI_PACK_TAG_BOOL);
    CHECK(fi->get_i64(f, "pass", &i) == 0);      // fail-closed: bool is not i64
    CHECK(fi->get_bool(f, "threshold", &bv) == 0); // fail-closed: i64 is not bool
    const char* sp = nullptr; int32_t sl = 0;
    CHECK(fi->get_str(f, "label", &sp, &sl) == 1 && sl == 6 &&
          std::string(sp, (size_t)sl) == "part-A");
    const void* bp = nullptr; int32_t bl = 0;
    CHECK(fi->get_bin(f, "raw", &bp, &bl) == 1 && bl == 3 &&
          static_cast<const uint8_t*>(bp)[2] == 3);

    // Image entry: dims + zero-copy pixel span.
    xi_pack_image iv{};
    CHECK(fi->get_image(f, "gray", &iv) == 1);
    CHECK(iv.width == 4 && iv.height == 4 && iv.channels == 1 && iv.length == 16);
    CHECK(iv.pixels && static_cast<const uint8_t*>(iv.pixels)[6] == 210);

    // Nested msgpack pass-through, decoded back.
    const void* mp = nullptr; int32_t ml = 0;
    CHECK(fi->get_mp(f, "pts", &mp, &ml) == 1 && ml > 0);
    {
        xi::mp::Reader r(static_cast<const uint8_t*>(mp), (size_t)ml);
        xi::mp::Element e;
        CHECK(r.next(e) == xi::mp::Status::Ok && e.kind == xi::mp::Kind::Array && e.len == 1);
        CHECK(r.next(e) == xi::mp::Status::Ok && e.kind == xi::mp::Kind::Map && e.len == 2);
    }

    // Fail-closed getters: absent key, and a type mismatch (i64 read of a str).
    int64_t junk;
    CHECK(fi->get_i64(f, "nope", &junk) == 0);
    CHECK(fi->get_i64(f, "label", &junk) == 0);       // wrong tag -> 0, no coercion
    CHECK(fi->tag_of(f, "nope") == -1);
    CHECK(fi->tag_of(f, "threshold") == XI_PACK_TAG_I64);
    CHECK(fi->tag_of(f, "gray") == XI_PACK_TAG_IMAGE);

    // Generic enumeration (the expose/record_save walk): count + key_at/tag_at.
    CHECK(fi->count(f) == 7);
    int32_t klen = 0;
    const char* k0 = fi->key_at(f, 0, &klen);
    CHECK(k0 && klen == 9 && std::string(k0, (size_t)klen) == "threshold");
    CHECK(fi->tag_at(f, 0) == XI_PACK_TAG_I64);
    CHECK(fi->tag_at(f, 2) == XI_PACK_TAG_BOOL);   // "pass" (insertion order)
    CHECK(fi->tag_at(f, 5) == XI_PACK_TAG_IMAGE);
    CHECK(fi->key_at(f, 99, &klen) == nullptr && klen == 0);   // OOB
    CHECK(fi->tag_at(f, 99) == -1);

    // Release: the pack drops, its pool handle frees, pool balances to baseline.
    fi->release(f);
    CHECK(pool_live() == base);
    // The handle is dead now — a getter on it is a safe 0, not a crash.
    CHECK(fi->get_i64(f, "threshold", &junk) == 0);
}

// ---------------------------------------------------------------------------
// (3) Refcount lifecycle: retain holds the pack alive past one release.
// ---------------------------------------------------------------------------
static void test_refcount_lifecycle() {
    SECTION("retain/release refcount + sealed-pack liveness");
    xi::install_pack_abi();
    const xi_pack_v1* fi = xi::pack_v1_iface();
    size_t base_frames = xi::PackRegistry::instance().live_frames();

    xi_pack_builder b = fi->builder_new();
    fi->builder_add_i64(b, "n", 7);
    xi_pack_handle f = fi->builder_seal(b);                 // rc = 1
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames + 1);

    fi->retain(f);                                           // rc = 2
    int64_t v;
    fi->release(f);                                          // rc = 1 (still alive)
    CHECK(fi->get_i64(f, "n", &v) == 1 && v == 7);
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames + 1);

    fi->release(f);                                          // rc = 0 (gone)
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames);
    CHECK(fi->get_i64(f, "n", &v) == 0);

    // Abandon an unsealed builder — no leak, nothing in the pack table.
    xi_pack_builder b2 = fi->builder_new();
    fi->builder_add_str(b2, "x", "y", 1);
    fi->builder_abandon(b2);
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames);
}

// ---------------------------------------------------------------------------
// (4) Dispatch: a Pack rides the bus via emit_pack. THE CUT (v12): the Record
//     currency is gone — TriggerEvent carries ONLY a pack (its image + metadata
//     ride inside the pack), so this exercises the sole dispatch path and proves
//     the pack + its pooled image balance to baseline after the event is dropped.
// ---------------------------------------------------------------------------
static void test_dispatch_dual_carry() {
    SECTION("TriggerBus carries a Pack (emit_pack); image rides the pack, balances to baseline");
    xi::install_pack_abi();                // wires the bus emit_pack forwarder + pack releaser
    const xi_pack_v1* fi = xi::pack_v1_iface();

    int base = pool_live();

    // A sink that captures every dispatched event (the inspection worker stand-in).
    std::vector<xi::TriggerEvent> got;
    xi::TriggerBus::instance().set_sink([&](xi::TriggerEvent ev) { got.push_back(std::move(ev)); });

    // Build a pack with an image + a scalar, emit it onto the bus.
    std::vector<uint8_t> gray(16, 0); gray[0] = 255;
    xi_pack_builder b = fi->builder_new();
    fi->builder_add_i64(b, "seq", 11);
    fi->builder_add_image(b, "frame", 4, 4, 1, gray.data());
    xi_pack_handle f = fi->builder_seal(b);        // emitter's ref (rc 1)
    fi->emit_pack("cam_frame", XI_TRIGGER_NULL, f, 0);  // event takes a 2nd ref
    fi->release(f);                                 // drop the emitter's ref (rc 1, held by event)

    CHECK(got.size() == 1);

    const xi::TriggerEvent* fe = got.empty() ? nullptr : &got.front();
    CHECK(fe != nullptr);
    if (fe) {
        CHECK(fe->is_real());                        // pack payload present
        CHECK(fe->pack != XI_PACK_NULL);
        CHECK(fe->leader_source == "cam_frame");
        int64_t seq = 0;
        CHECK(fi->get_i64(fe->pack, "seq", &seq) == 1 && seq == 11);
        xi_pack_image iv{};
        CHECK(fi->get_image(fe->pack, "frame", &iv) == 1 && iv.width == 4);
    }

    // Release the event exactly as the dispatcher's release_trigger_event_ does.
    for (auto& ev : got) {
        if (ev.pack != XI_PACK_NULL) xi::TriggerBus::instance().release_pack_(ev.pack);
    }
    got.clear();
    xi::TriggerBus::instance().clear_sink();

    CHECK(pool_live() == base);   // the pack + its pooled image ref accounted for
}

// ---------------------------------------------------------------------------
// (5) Owner-tagged sweep — the PackRegistry analogue of ImagePool's
//     release_all_for (pack-plane hardening; the cache plugin's registry
//     finding). THE ORIGINAL ASYMMETRIC SCENARIO: a pack-retaining plugin
//     (cache's ring) retains a sealed host pack under its OwnerGuard and is
//     destroyed WITHOUT releasing. Pre-hardening the sealed pack — and the
//     pool image it holds — leaked in the registry silently (no sweep, no
//     diagnostic) until static teardown. Now the teardown sweep reclaims and
//     counts it, and a co-owner's live ref is never touched.
// ---------------------------------------------------------------------------
static void test_owner_sweep_regression() {
    SECTION("owner-tagged sweep reclaims a forgotten pack ref; co-owners untouched");
    xi::install_pack_abi();
    const xi_pack_v1* fi = xi::pack_v1_iface();
    size_t base_frames = xi::PackRegistry::instance().live_frames();
    int    base_live   = pool_live();

    xi::ImagePoolOwnerId P = xi::ImagePool::alloc_owner_id();   // producer instance
    xi::ImagePoolOwnerId C = xi::ImagePool::alloc_owner_id();   // pack-retaining consumer (the cache role)

    // Producer: build + seal under its OwnerGuard, exactly as the adapter wraps
    // a source's process(). The creator's initial ref is tagged to P.
    std::vector<uint8_t> gray(16, 7);
    xi_pack_handle f = XI_PACK_NULL;
    {
        xi::ImagePool::OwnerGuard g(P);
        xi_pack_builder b = fi->builder_new();
        fi->builder_add_i64(b, "seq", 1);
        fi->builder_add_image(b, "frame", 4, 4, 1, gray.data());
        f = fi->builder_seal(b);
    }
    CHECK(f != XI_PACK_NULL);
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames + 1);
    CHECK(xi::PackRegistry::instance().owner_refs(P) == 1);
    CHECK(pool_live() == base_live + 1);              // the pack's pooled image

    // Consumer: retains the incoming sealed pack into its ring under ITS guard
    // (the cache capture path).
    { xi::ImagePool::OwnerGuard g(C); fi->retain(f); }
    CHECK(xi::PackRegistry::instance().owner_refs(C) == 1);

    // The producer releases its own ref properly -> its ledger drains; a sweep
    // of the WELL-BEHAVED owner reclaims nothing and disturbs nothing.
    { xi::ImagePool::OwnerGuard g(P); fi->release(f); }
    CHECK(xi::PackRegistry::instance().owner_refs(P) == 0);
    CHECK(xi::ImagePool::sweep_packs_for(P) == 0);
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames + 1);  // C keeps it alive
    int64_t v = 0;
    CHECK(fi->get_i64(f, "seq", &v) == 1 && v == 1);

    // THE ASYMMETRY, REGRESSED: C dies without releasing. The sweep (what the
    // adapter dtor now calls through ImagePool::sweep_packs_for) reclaims the
    // forgotten ref, frees the pack AND its pool image, and reports the count.
    int swept = xi::ImagePool::sweep_packs_for(C);
    CHECK(swept == 1);
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames);
    CHECK(pool_live() == base_live);                  // pooled image freed with the pack
    CHECK(fi->get_i64(f, "seq", &v) == 0);            // the handle is dead

    // Co-owner protection: one leaking consumer, one live one. Sweeping the
    // leaker drops ONLY its ref; the live consumer's pack stays readable.
    xi::ImagePoolOwnerId Ca = xi::ImagePool::alloc_owner_id();  // will leak
    xi::ImagePoolOwnerId Cb = xi::ImagePool::alloc_owner_id();  // well-behaved
    xi_pack_handle f2 = XI_PACK_NULL;
    {
        xi::ImagePool::OwnerGuard g(P);
        xi_pack_builder b = fi->builder_new();
        fi->builder_add_i64(b, "n", 42);
        f2 = fi->builder_seal(b);
    }
    { xi::ImagePool::OwnerGuard g(Ca); fi->retain(f2); }
    { xi::ImagePool::OwnerGuard g(Cb); fi->retain(f2); }
    { xi::ImagePool::OwnerGuard g(P);  fi->release(f2); }
    CHECK(xi::ImagePool::sweep_packs_for(Ca) == 1);   // the leaker's ref only
    CHECK(fi->get_i64(f2, "n", &v) == 1 && v == 42);  // Cb's pack still alive
    { xi::ImagePool::OwnerGuard g(Cb); fi->release(f2); }
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames);

    // Untagged framework refs are never charged to a plugin: an emit's event
    // ref (retain_untagged) does not appear in any owner ledger.
    xi_pack_handle f3 = XI_PACK_NULL;
    {
        xi::ImagePool::OwnerGuard g(P);
        xi_pack_builder b = fi->builder_new();
        fi->builder_add_i64(b, "k", 9);
        f3 = fi->builder_seal(b);
    }
    xi::PackRegistry::instance().retain_untagged(f3);  // the event's ref
    CHECK(xi::PackRegistry::instance().owner_refs(P) == 1);   // creator ref only
    { xi::ImagePool::OwnerGuard g(P); fi->release(f3); }      // producer done
    CHECK(xi::ImagePool::sweep_packs_for(P) == 0);            // nothing charged to P
    xi::PackRegistry::instance().release(f3);                 // dispatcher drops the event ref
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames);
}

// ---------------------------------------------------------------------------
// (6) H1 regression: t.has_source() / t.sources() are honest for a PACK-plane
//     trigger. The frame rides the pack, so the Record-plane image map is empty
//     — keying has_source() off it alone always answered false on the pack path
//     (qa_multi_graph had to route via t.primary_source()). The source identity
//     of a pack trigger is its leader_source (emitting instance) + the pack's
//     own $src stamp; both must resolve true, a stranger false.
//     THE CUT (v12): the Record path (pack == NULL, image-map source) was removed
//     with the Record data plane, so only the pack path is exercised here.
// ---------------------------------------------------------------------------
static void test_has_source_pack_identity() {
    SECTION("H1: has_source()/sources() honest on the pack path");
    xi::install_pack_abi();
    xi_host_api host = xi::ImagePool::make_host_api();
    const xi_pack_v1* fi = xi::pack_v1_iface();
    int base = pool_live();

    // --- PACK path: a sealed pack with a frame + a $src producer stamp. The
    //     event's leader_source is the emitting instance ("camA"); the pack was
    //     minted by "detA" ($src). image_count = 0 (the frame rides the pack). ---
    std::vector<uint8_t> gray(16, 0); gray[0] = 255;
    xi_pack_builder b = fi->builder_new();
    fi->builder_add_i64(b, "seq", 7);
    fi->builder_add_image(b, "frame", 4, 4, 1, gray.data());
    fi->builder_add_str(b, "$src", "detA", 4);
    xi_pack_handle f = fi->builder_seal(b);          // rc 1 (ours)
    CHECK(f != XI_PACK_NULL);

    {
        xi_trigger_view v{};
        v.is_active     = 1;
        v.id            = xi_trigger_id{ 0xABCD, 0x1234 };
        v.leader_source = "camA";
        v.images        = nullptr;
        v.image_count   = 0;                          // pack-plane: no image map
        v.pack          = f;                          // borrowed; Trigger takes its own ref
        v.host          = &host;

        xi::Trigger t(&v);
        CHECK(t.is_active());
        CHECK((bool)t.pack());                        // the pack came through
        CHECK(t.primary_source() == "camA");          // leader, as today

        // THE FIX: honest on the pack path (both were false before H1).
        CHECK(t.has_source("camA"));                  // leader_source
        CHECK(t.has_source("detA"));                  // pack $src producer stamp
        CHECK(!t.has_source("camB"));                 // a stranger is still false
        CHECK(!t.has_source(nullptr));                // null-safe

        // sources() reports the same identity instead of silently empty.
        auto srcs = t.sources();
        bool has_camA = false, has_detA = false;
        for (auto& s : srcs) { has_camA |= (s == "camA"); has_detA |= (s == "detA"); }
        CHECK(srcs.size() == 2 && has_camA && has_detA);
    }
    fi->release(f);                                    // drop our ref (Trigger dropped its own)
    CHECK(pool_live() == base);                        // pack + pooled image balanced
}

int main() {
    std::printf("[test] xi.pack@1 carved data-plane door + dispatch dual-carry\n");
    test_door_probe();
    test_build_read_roundtrip();
    test_refcount_lifecycle();
    test_dispatch_dual_carry();
    test_owner_sweep_regression();
    test_has_source_pack_identity();
    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
