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
//   5.-8. Creator-tag owner sweep, H1 pack source identity, F1 pool-exhaustion
//      honesty, cross-plane owner-sweep co-ownership (see each section).
//   9. The xi.pack@4 self-describing blob door end-to-end (blob_mint -> fill ->
//      adopt_blob zero-copy path / add_blob copy / get_blob / the @1 get_image
//      adapter over an xi/image blob / ordinal entry walk), driven plugin-style
//      through host.get_interface("xi.pack", 4).
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
    CHECK(host.get_interface("xi.pack", 2) == nullptr);  // @2 never existed
    CHECK(host.get_interface("xi.pack", 0) == nullptr);
    CHECK(host.get_interface("xi.packX", 1) == nullptr);

    // A pack-capable plugin caches this pointer once (Plugin::pack_iface()).
    const auto* fi = static_cast<const xi_pack_v1*>(v1);
    CHECK(fi->builder_new && fi->builder_seal && fi->get_i64 && fi->emit_pack);

    // blob plane (spec 30): the xi.pack@4 supplement resolves alongside @1
    // (Plugin::pack4_iface()) — same id, version 4, its own frozen vtable. The
    // retired @3 answers NULL forever.
    CHECK(host.get_interface("xi.pack", 3) == nullptr);
    const void* v4 = host.get_interface("xi.pack", 4);
    CHECK(v4 != nullptr);
    CHECK(v4 == xi::pack_v4_iface());
    CHECK(v4 != v1);
    const auto* fi4 = static_cast<const xi_pack_v4*>(v4);
    CHECK(fi4->blob_mint && fi4->builder_adopt_blob && fi4->builder_add_blob &&
          fi4->get_blob && fi4->entry_at);
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
    // The image entry is a self-describing "xi/image" BLOB now (the frozen @1
    // get_image slot is a door adapter that parses it); tag_of reports BLOB.
    CHECK(fi->tag_of(f, "gray") == XI_PACK_TAG_BLOB);

    // Generic enumeration (the expose/record_save walk): count + key_at/tag_at.
    CHECK(fi->count(f) == 7);
    int32_t klen = 0;
    const char* k0 = fi->key_at(f, 0, &klen);
    CHECK(k0 && klen == 9 && std::string(k0, (size_t)klen) == "threshold");
    CHECK(fi->tag_at(f, 0) == XI_PACK_TAG_I64);
    CHECK(fi->tag_at(f, 2) == XI_PACK_TAG_BOOL);   // "pass" (insertion order)
    CHECK(fi->tag_at(f, 5) == XI_PACK_TAG_BLOB);   // "gray" is an xi/image blob
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
// (5) Creator-tag sweep — the PackRegistry analogue of ImagePool's
//     release_all_for, on the SINGLE-CREATOR-TAG model: the only owner-tracked
//     ref is the creator's seal ref. A producer that seals and forgets to
//     release is swept on destroy (the leak diagnostic); a consumer's retain is
//     an UNTRACKED ++rc, so a consumer that leaks is DIAGNOSED (live_frames),
//     never swept — the sweep drops at most one ref per slot and can never
//     free a pack out from under a live holder.
// ---------------------------------------------------------------------------
static void test_owner_sweep_regression() {
    SECTION("creator-tag sweep: producer leak reclaimed; consumer leak diagnosed, never UAF");
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
    CHECK(xi::PackRegistry::instance().owner_refs(P) == 1);   // the creator tag
    CHECK(pool_live() == base_live + 1);              // the pack's pooled image

    // Consumer: retains the incoming sealed pack into its ring under ITS guard
    // (the cache capture path). Untracked — no owner charge.
    { xi::ImagePool::OwnerGuard g(C); fi->retain(f); }
    CHECK(xi::PackRegistry::instance().owner_refs(C) == 0);   // consumer refs untracked

    // The producer releases its own ref properly -> its creator tag clears; a
    // sweep of the WELL-BEHAVED owner reclaims nothing and disturbs nothing.
    { xi::ImagePool::OwnerGuard g(P); fi->release(f); }
    CHECK(xi::PackRegistry::instance().owner_refs(P) == 0);
    CHECK(xi::ImagePool::sweep_packs_for(P) == 0);
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames + 1);  // C keeps it alive
    int64_t v = 0;
    CHECK(fi->get_i64(f, "seq", &v) == 1 && v == 1);

    // CONSUMER-RETAIN LEAK: C dies without releasing. Its sweep (what the
    // adapter dtor calls through ImagePool::sweep_packs_for) reclaims NOTHING —
    // creator != C, and consumer refs are untracked by design. The pack must
    // neither vanish (no over-release) nor dangle: it stays a live, readable,
    // DIAGNOSED leak (live_frames), reclaimed only at process teardown.
    int swept = xi::ImagePool::sweep_packs_for(C);
    CHECK(swept == 0);
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames + 1);  // diagnosed leak
    CHECK(pool_live() == base_live + 1);              // pooled image rides the live pack
    CHECK(fi->get_i64(f, "seq", &v) == 1 && v == 1);  // no UAF: still readable
    // (Test hygiene: drop the leaked ref so the table balances — in production
    // this ref survives to process exit, by design.)
    xi::PackRegistry::instance().release_as(f, 0);
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames);
    CHECK(pool_live() == base_live);
    CHECK(fi->get_i64(f, "seq", &v) == 0);            // the handle is dead now

    // PRODUCER LEAK + live consumer: the creator P seals and FORGETS to
    // release; consumer Cb holds. Sweeping P reclaims exactly the creator's one
    // seal ref (the leak diagnostic) and the pack survives for Cb — the sweep
    // is incapable of dropping more than that one ref.
    xi::ImagePoolOwnerId Cb = xi::ImagePool::alloc_owner_id();  // well-behaved consumer
    xi_pack_handle f2 = XI_PACK_NULL;
    {
        xi::ImagePool::OwnerGuard g(P);
        xi_pack_builder b = fi->builder_new();
        fi->builder_add_i64(b, "n", 42);
        f2 = fi->builder_seal(b);
    }
    { xi::ImagePool::OwnerGuard g(Cb); fi->retain(f2); }
    CHECK(xi::ImagePool::sweep_packs_for(P) == 1);    // the leaked seal ref only
    CHECK(fi->get_i64(f2, "n", &v) == 1 && v == 42);  // Cb's pack still alive
    CHECK(xi::ImagePool::sweep_packs_for(P) == 0);    // redundant sweep: no-op
    { xi::ImagePool::OwnerGuard g(Cb); fi->release(f2); }
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames);

    // Untagged framework refs are never charged to a plugin: an emit's event
    // ref (retain_untagged) is a plain ++rc alongside the creator tag.
    xi_pack_handle f3 = XI_PACK_NULL;
    {
        xi::ImagePool::OwnerGuard g(P);
        xi_pack_builder b = fi->builder_new();
        fi->builder_add_i64(b, "k", 9);
        f3 = fi->builder_seal(b);
    }
    xi::PackRegistry::instance().retain_untagged(f3);  // the event's ref
    CHECK(xi::PackRegistry::instance().owner_refs(P) == 1);   // creator tag only
    { xi::ImagePool::OwnerGuard g(P); fi->release(f3); }      // producer done, tag clears
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

// ---------------------------------------------------------------------------
// (7) F1 regression: a large (pooled-class) bin added while the pool is
//     EXHAUSTED must never surface a {nullptr, len>0} span. Pre-fix, add_bin
//     stored {pooled=true, handle=0, inl_len=n} on a failed pool alloc, and
//     get_bin did view(0).first(n) -> a span{nullptr, n}; f_get_bin then
//     reported rc=1 (SUCCESS) with ptr=nullptr, len=n -> the consumer OOB-reads
//     n bytes from nullptr. The fix (producer honesty) falls back to INLINE
//     arena storage on pool failure, so the bytes still ride truthfully; the
//     consumer-side get_bin guard additionally refuses to build a poisoned span.
//     This exercises the real exhaustion path end-to-end through the C ABI.
// ---------------------------------------------------------------------------
static void test_bin_pool_exhaustion_no_null_span() {
    SECTION("F1: large bin under pool exhaustion never yields success-with-null");
    xi::install_pack_abi();
    const xi_pack_v1* fi = xi::pack_v1_iface();

    // Drain the ImagePool to exhaustion with minimal (1x1x1) handles so the
    // next alloc_bytes() inside add_bin is forced to fail (handle==0).
    std::vector<xi_image_handle> hog;
    hog.reserve(70000);
    for (;;) {
        xi_image_handle h = xi::ImagePool::instance().create(1, 1, 1);
        if (!h) break;                 // pool exhausted -> the failure we want
        hog.push_back(h);
    }
    CHECK(!hog.empty());               // we actually filled it
    // Confirm the pool is genuinely exhausted right now.
    CHECK(xi::ImagePool::instance().create(1, 1, 1) == XI_IMAGE_NULL);

    // A pooled-class payload (>= kPackLargeThreshold = 4096) built while the
    // pool cannot mint a buffer.
    const int32_t N = 8192;
    std::vector<uint8_t> payload(N);
    for (int32_t i = 0; i < N; ++i) payload[i] = uint8_t(i * 7 + 3);

    xi_pack_builder b = fi->builder_new();
    fi->builder_add_bin(b, "payload", payload.data(), N);
    xi_pack_handle f = fi->builder_seal(b);
    CHECK(f != XI_PACK_NULL);

    const void* ptr = reinterpret_cast<const void*>(0x1);   // poison sentinel
    int32_t     len = -1;
    int32_t     rc  = fi->get_bin(f, "payload", &ptr, &len);

    // THE INVARIANT: never success-with-null. rc==1 => ptr must be non-null and
    // len==N with the exact bytes; rc==0 (truthful "absent") is also acceptable,
    // but must NOT hand back ptr=nullptr, len>0.
    CHECK(!(rc == 1 && ptr == nullptr && len > 0));   // the F1 crash condition
    if (rc == 1) {
        CHECK(ptr != nullptr);
        CHECK(len == N);
        bool bytes_ok = (ptr != nullptr && len == N);
        if (bytes_ok) {
            const uint8_t* p = static_cast<const uint8_t*>(ptr);
            for (int32_t i = 0; i < N && bytes_ok; ++i)
                bytes_ok = (p[i] == uint8_t(i * 7 + 3));
        }
        CHECK(bytes_ok);   // producer-honesty: the data rode inline, intact
    }

    fi->release(f);

    // Restore the pool for any subsequent test.
    for (xi_image_handle h : hog) xi::ImagePool::instance().release(h);
}

// ---------------------------------------------------------------------------
// (8) Cross-plane owner-sweep regression: the producer's IMAGE-plane sweep
//     (ImagePool::release_all_for, what the adapter dtor runs on instance
//     teardown) must not free a pool buffer minted INTO a pack that a consumer
//     still co-owns via a PACK-level retain (the shipped cache/buffer_replay
//     pattern — fi->retain on the whole pack, no per-image adopt).
//
//     Pre-fix: pack_pool minted with owner = current_owner() (the producer P)
//     at pool rc 1, and PackRegistry::retain bumps only the pack-registry rc —
//     never the pool rc. So release_all_for(P) hit the rc 1->0 branch and freed
//     the buffer while the registry's R1 guard correctly kept the pack alive
//     for the co-owner: the pack survived but get_image returned an empty span
//     (generation-checked, memory-safe) — silent data loss. Post-fix the pack's
//     buffers are minted owner-NEUTRAL (0): the image sweep skips them (swept
//     == 0) and the pack alone governs their release. This test FAILS pre-fix
//     (swept == 1, iv.pixels == null/length 0) and PASSES post-fix.
// ---------------------------------------------------------------------------
static void test_cross_plane_owner_sweep_keeps_coowned_pack_buffers() {
    SECTION("cross-plane: producer image-sweep spares a co-owned pack's minted buffers");
    xi::install_pack_abi();
    const xi_pack_v1* fi = xi::pack_v1_iface();
    size_t base_frames = xi::PackRegistry::instance().live_frames();
    int    base_live   = pool_live();

    xi::ImagePoolOwnerId P = xi::ImagePool::alloc_owner_id();   // producer instance
    xi::ImagePoolOwnerId C = xi::ImagePool::alloc_owner_id();   // pack-retaining consumer

    // Producer: mint an image AND a pooled-class bin INTO the pack under its
    // OwnerGuard, exactly as the adapter wraps a source's process().
    std::vector<uint8_t> gray(16);
    for (int i = 0; i < 16; ++i) gray[(size_t)i] = uint8_t(i * 11 + 5);
    const int32_t BN = 4096;                       // >= kPackLargeThreshold -> pooled
    std::vector<uint8_t> big((size_t)BN);
    for (int32_t i = 0; i < BN; ++i) big[(size_t)i] = uint8_t(i * 13 + 1);
    xi_pack_handle f = XI_PACK_NULL;
    {
        xi::ImagePool::OwnerGuard g(P);
        xi_pack_builder b = fi->builder_new();
        fi->builder_add_i64(b, "seq", 5);
        fi->builder_add_image(b, "frame", 4, 4, 1, gray.data());
        fi->builder_add_bin(b, "blob", big.data(), BN);
        f = fi->builder_seal(b);
    }
    CHECK(f != XI_PACK_NULL);
    CHECK(pool_live() == base_live + 2);           // image + pooled bin minted

    // Consumer co-owns the PACK across frames (cache.cpp's fi->retain — a
    // pack-LEVEL retain; it does NOT addref the pack's pool buffers).
    { xi::ImagePool::OwnerGuard g(C); fi->retain(f); }

    // Producer releases its own pack ref properly (well-behaved source), then
    // is torn down: the adapter dtor's IMAGE-plane leak sweep runs.
    { xi::ImagePool::OwnerGuard g(P); fi->release(f); }
    int swept = xi::ImagePool::instance().release_all_for(P);
    CHECK(swept == 0);                             // pre-fix: 2 (pack buffers freed as "P's leaks")
    CHECK(xi::ImagePool::sweep_packs_for(P) == 0); // pack plane: nothing charged to P
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames + 1);  // C keeps the pack

    // THE DATA-LOSS ASSERTION: the co-owned pack's buffers are still readable.
    xi_pack_image iv{};
    CHECK(fi->get_image(f, "frame", &iv) == 1);
    CHECK(iv.width == 4 && iv.height == 4 && iv.channels == 1);
    CHECK(iv.length == 16 && iv.pixels != nullptr);     // pre-fix: length 0 / null
    if (iv.pixels && iv.length == 16) {
        const uint8_t* p = static_cast<const uint8_t*>(iv.pixels);
        bool ok = true;
        for (int i = 0; i < 16; ++i) ok = ok && (p[i] == uint8_t(i * 11 + 5));
        CHECK(ok);                                      // pixels intact, not just non-null
    }
    const void* bp = nullptr; int32_t bl = 0;
    CHECK(fi->get_bin(f, "blob", &bp, &bl) == 1);       // pre-fix: 0 (F1 guard refuses dead span)
    CHECK(bp != nullptr && bl == BN);
    if (bp && bl == BN) {
        const uint8_t* p = static_cast<const uint8_t*>(bp);
        bool ok = true;
        for (int32_t i = 0; i < BN; ++i) ok = ok && (p[i] == uint8_t(i * 13 + 1));
        CHECK(ok);
    }

    // The co-owner drops its last ref: pack destroyed, buffers freed exactly
    // once (no leak, no double-free), everything balances to baseline.
    { xi::ImagePool::OwnerGuard g(C); fi->release(f); }
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames);
    CHECK(pool_live() == base_live);
    CHECK(fi->get_image(f, "frame", &iv) == 0);         // the handle is dead now
}

// ---------------------------------------------------------------------------
// (9) The xi.pack@4 self-describing blob door end-to-end, plugin-style: BOTH
//     vtables resolved via host.get_interface (exactly what a pack-capable
//     plugin does at create), building/reading through the SAME builder/handle
//     ids as v1. Covers: blob_mint -> fill payload in place -> adopt_blob (the
//     zero-copy producer path, 64B-aligned payload), add_blob copy convenience,
//     get_blob (descriptor + payload), the @1 get_image adapter over an
//     "xi/image" blob (+ fail-closed on a non-image blob and on get_bin), and
//     ordinal entry_at parity with v1 key_at/tag_at.
// ---------------------------------------------------------------------------
static void test_pack_v4_door() {
    SECTION("xi.pack@4: blob mint/adopt/add/get + image adapter + ordinal walk");
    xi::install_pack_abi();
    xi_host_api host = xi::ImagePool::make_host_api();
    const auto* fi  = static_cast<const xi_pack_v1*>(host.get_interface("xi.pack", 1));
    const auto* fi4 = static_cast<const xi_pack_v4*>(host.get_interface("xi.pack", 4));
    CHECK(fi != nullptr && fi4 != nullptr);
    size_t base_frames = xi::PackRegistry::instance().live_frames();
    int    base_live   = pool_live();

    // An xi/image descriptor for a 3x2x1 u8 blob:
    //   {"t":"xi/image","w":3,"h":2,"c":1,"dt":"u8"}
    xi::mp::Writer dw;
    dw.map(5);
    dw.key("t");  dw.str("xi/image");
    dw.key("w");  dw.int_(3);
    dw.key("h");  dw.int_(2);
    dw.key("c");  dw.int_(1);
    dw.key("dt"); dw.str("u8");
    const xi::mp::Bytes desc(dw.bytes());
    const int64_t payload_len = 3 * 2 * 1;

    xi_pack_builder b = fi->builder_new();
    fi->builder_add_i64(b, "seq", 3);

    // (a) blob_mint -> fill the 64B-aligned payload IN PLACE -> adopt_blob.
    void* pptr = nullptr;
    xi_image_handle h = fi4->blob_mint(desc.data(), (int32_t)desc.size(),
                                       payload_len, &pptr);
    CHECK(h != XI_IMAGE_NULL && pptr != nullptr);
    CHECK((reinterpret_cast<uintptr_t>(pptr) & 63u) == 0);   // payload 64B-aligned
    uint8_t* px = static_cast<uint8_t*>(pptr);
    for (int i = 0; i < 6; ++i) px[i] = uint8_t(0x10 + i);
    CHECK(fi4->builder_adopt_blob(b, "img", h) == 1);        // pack co-owns (addref)
    host.image_release(h);                                   // caller drops its mint ref
    CHECK(pool_live() == base_live + 1);                     // one pooled blob buffer

    // (b) add_blob copy convenience, a custom convention type.
    xi::mp::Writer dw2;
    dw2.map(2); dw2.key("t"); dw2.str("acme/roi"); dw2.key("n"); dw2.int_(4);
    const xi::mp::Bytes desc2(dw2.bytes());
    const uint8_t roi[4] = { 9, 8, 7, 6 };
    CHECK(fi4->builder_add_blob(b, "roi", desc2.data(), (int32_t)desc2.size(), roi, 4) == 1);
    // An invalid (non-map) descriptor is refused, nothing added.
    xi::mp::Writer bad; bad.int_(7);
    CHECK(fi4->builder_add_blob(b, "bad", bad.bytes().data(),
                                (int32_t)bad.bytes().size(), roi, 4) == 0);
    CHECK(pool_live() == base_live + 2);

    xi_pack_handle A = fi->builder_seal(b);
    CHECK(A != XI_PACK_NULL);

    // A blob's tag is XI_PACK_TAG_BLOB. v1 get_bin fails closed on it; the @1
    // get_image adapter parses an "xi/image" blob and rejects a non-image one.
    CHECK(fi->tag_of(A, "img") == XI_PACK_TAG_BLOB);
    CHECK(fi->tag_of(A, "roi") == XI_PACK_TAG_BLOB);
    const void* jp = nullptr; int32_t jl = 0;
    CHECK(fi->get_bin(A, "img", &jp, &jl) == 0);             // a blob is not a plain bin
    xi_pack_image iv{};
    CHECK(fi->get_image(A, "img", &iv) == 1);                // xi/image adapter parses it
    CHECK(iv.width == 3 && iv.height == 2 && iv.channels == 1 && iv.length == 6);
    CHECK(iv.pixels && static_cast<const uint8_t*>(iv.pixels)[5] == 0x15);
    CHECK(fi->get_image(A, "roi", &iv) == 0);                // not an xi/image blob

    // @4 get_blob: descriptor + payload, both zero-copy.
    const void* dptr = nullptr; int32_t dlen = 0;
    const void* yptr = nullptr; int64_t ylen = 0;
    CHECK(fi4->get_blob(A, "img", &dptr, &dlen, &yptr, &ylen) == 1);
    CHECK(dptr && dlen == (int32_t)desc.size() &&
          std::memcmp(dptr, desc.data(), desc.size()) == 0);
    CHECK(yptr && ylen == payload_len &&
          static_cast<const uint8_t*>(yptr)[0] == 0x10);
    CHECK(fi4->get_blob(A, "seq",  &dptr, &dlen, &yptr, &ylen) == 0);  // i64 is not a blob
    CHECK(fi4->get_blob(A, "nope", &dptr, &dlen, &yptr, &ylen) == 0);
    CHECK(fi4->get_blob(A, "roi",  &dptr, &dlen, &yptr, &ylen) == 1);
    CHECK(ylen == 4 && static_cast<const uint8_t*>(yptr)[0] == 9);

    // ---- ordinal walk: entry_at parity with v1 key_at/tag_at (no type_id) ----
    CHECK(fi->count(A) == 3);                                // seq, img, roi (bad refused)
    for (int32_t i = 0; i < 3; ++i) {
        int32_t kl = 0;
        const char* k = fi->key_at(A, i, &kl);
        xi_pack_entry e{};
        CHECK(fi4->entry_at(A, i, &e) == 1);
        CHECK(e.key && k && e.key_len == kl &&
              std::memcmp(e.key, k, (size_t)kl) == 0);       // same insertion order
        CHECK(e.tag == fi->tag_at(A, i));
    }
    xi_pack_entry e0{};
    CHECK(fi4->entry_at(A, 0, &e0) == 1 && e0.external == 0 &&
          e0.tag == XI_PACK_TAG_I64);                        // "seq": inline
    xi_pack_entry e1{};
    CHECK(fi4->entry_at(A, 1, &e1) == 1 && e1.external == 1 &&
          e1.tag == XI_PACK_TAG_BLOB);                       // "img": pooled blob
    CHECK(fi4->entry_at(A, 99, &e0) == 0);                   // OOB

    // blob_mint refuses a non-canonical / non-map descriptor (nothing minted).
    void* pp = reinterpret_cast<void*>(0x1);
    CHECK(fi4->blob_mint(bad.bytes().data(), (int32_t)bad.bytes().size(), 4, &pp)
          == XI_IMAGE_NULL);
    CHECK(pp == nullptr);

    fi->release(A);
    CHECK(pool_live() == base_live);                         // exactly-once release, no leak
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames);
}

// ---------------------------------------------------------------------------
// (10) Finding ⑦ regression: foreign msgpack is canonicalized AT THE C-ABI SEAM
//      (builder_add_mp), turning "ingress is the only path" from a convention
//      into structure. Well-formed canonical bytes round-trip byte-identical
//      (canonicalize is idempotent, so wire/golden bytes are unaffected);
//      malformed / ext-bearing (incl. handle-shaped) bytes are REFUSED — nothing
//      is stored, matching ScriptPack::add_mp's fail-closed drop. Pre-fix this
//      trampoline copied caller bytes verbatim into the slab, so hostile msgpack
//      could ride the wire / a replay file.
// ---------------------------------------------------------------------------
static void test_add_mp_seam_canonicalize() {
    SECTION("⑦: builder_add_mp canonicalizes foreign msgpack at the C-ABI seam");
    xi::install_pack_abi();
    const xi_pack_v1* fi = xi::pack_v1_iface();

    // Well-formed canonical value (an xi::mp::Writer value is canonical by
    // construction — max-width tags, string keys).
    xi::mp::Writer mw;
    mw.array(2); mw.int_(1); mw.str("hi");
    const xi::mp::Bytes canon(mw.bytes());   // copy: seal recycles the writer's buffer path

    xi_pack_builder b = fi->builder_new();
    fi->builder_add_mp(b, "good", canon.data(), (int32_t)canon.size());

    // Malformed: a fixarray header claiming 4 elements with no payload — the
    // classic truncation the interior must never trust.
    const uint8_t truncated[1] = { 0x94 };
    fi->builder_add_mp(b, "trunc", truncated, (int32_t)sizeof truncated);

    // Handle-shaped ext (fixext1, type 0x70 = kPoolHandleExtType): a forged pool
    // ref. reject-all ext policy refuses it (ingress never imports a pool handle).
    const uint8_t handle_ext[3] = { 0xd4, 0x70, 0x00 };
    fi->builder_add_mp(b, "forged", handle_ext, (int32_t)sizeof handle_ext);

    // Any other foreign ext is likewise refused by the reject-all policy.
    const uint8_t any_ext[3] = { 0xd4, 0x01, 0x2a };
    fi->builder_add_mp(b, "ext", any_ext, (int32_t)sizeof any_ext);

    xi_pack_handle f = fi->builder_seal(b);
    CHECK(f != XI_PACK_NULL);

    // Only the well-formed entry survived; the three hostile ones were dropped.
    CHECK(fi->count(f) == 1);
    CHECK(fi->tag_of(f, "good") == XI_PACK_TAG_MP);
    CHECK(fi->tag_of(f, "trunc")  == -1);
    CHECK(fi->tag_of(f, "forged") == -1);
    CHECK(fi->tag_of(f, "ext")    == -1);

    // Byte-identical: canonical input re-emitted verbatim (idempotent) — wire
    // and golden bytes are unaffected by the seam gate.
    const void* mp = nullptr; int32_t ml = 0;
    CHECK(fi->get_mp(f, "good", &mp, &ml) == 1);
    CHECK(ml == (int32_t)canon.size());
    CHECK(mp && std::memcmp(mp, canon.data(), canon.size()) == 0);

    fi->release(f);
}

// ---------------------------------------------------------------------------
// (11) Finding ② regression (mechanism-level): the single-creator-tag invariant
//      requires the creator's seal ref to be retired UNDER OwnerGuard(creator).
//      reinit() destroys the OLD plugin instance, whose dtor releases the pack
//      refs it created; the fix wraps that destroy in OwnerGuard(owner_id_) so
//      those releases match the creator tag and clear it (exactly like the
//      adapter dtor path). This pins the invariant both ways: a GUARDED creator
//      release (what fixed reinit now does) clears the tag, so a later owner
//      sweep reclaims nothing and a consumer's co-held ref survives; an
//      UNGUARDED release (the pre-fix reinit path — destroy ran off-guard, so
//      release_as(pack, 0)) STRANDS the tag, and the same sweep then
//      over-releases the consumer's live ref — the UAF finding ② describes.
//
//      NOTE: this drives the PackRegistry mechanism directly rather than a real
//      reinit() through a DLL. A faithful reinit-driven test needs a plugin
//      whose dtor releases a pack ref it created (a cache/ring plugin); no such
//      test plugin exists and building one was out of scope ("no huge new
//      harness"). The fix itself (xi_cabi_adapter.hpp reinit) is a one-line
//      OwnerGuard mirroring the already-tested dtor path.
// ---------------------------------------------------------------------------
static void test_reinit_creator_tag_ownerguard() {
    SECTION("②: the creator seal ref must be retired under OwnerGuard(creator)");
    xi::install_pack_abi();
    const xi_pack_v1* fi = xi::pack_v1_iface();
    auto& reg = xi::PackRegistry::instance();
    size_t base_frames = reg.live_frames();

    xi::ImagePoolOwnerId X = xi::ImagePool::alloc_owner_id();   // the OLD instance (creator)
    xi::ImagePoolOwnerId Q = xi::ImagePool::alloc_owner_id();   // a consumer co-holding the pack

    // --- THE FIX: the old instance's dtor releases its seal ref UNDER
    //     OwnerGuard(owner_id_) (fixed reinit). The tag clears; the sweep is safe.
    {
        xi_pack_handle P = XI_PACK_NULL;
        { xi::ImagePool::OwnerGuard g(X);            // old instance seals under its guard
          xi_pack_builder b = fi->builder_new();
          fi->builder_add_i64(b, "seq", 7);
          P = fi->builder_seal(b); }
        CHECK(P != XI_PACK_NULL);
        CHECK(reg.owner_refs(X) == 1);               // creator tag live (rc 1)
        { xi::ImagePool::OwnerGuard g(Q); fi->retain(P); }   // consumer co-holds (rc 2, untracked)

        { xi::ImagePool::OwnerGuard g(X); fi->release(P); }  // GUARDED release → tag cleared, rc 1
        CHECK(reg.owner_refs(X) == 0);               // NOT stranded
        CHECK(xi::ImagePool::sweep_packs_for(X) == 0);       // later teardown sweep reclaims nothing
        int64_t v = 0;
        CHECK(fi->get_i64(P, "seq", &v) == 1 && v == 7);     // consumer's ref still valid — no UAF
        { xi::ImagePool::OwnerGuard g(Q); fi->release(P); }  // consumer done → freed exactly once
        CHECK(reg.live_frames() == base_frames);
    }

    // --- THE BUG the fix removes: an UNGUARDED creator release (pre-fix reinit's
    //     destroy ran with current_owner()==0 → release_as(pack, 0)). The tag
    //     clears only on owner match, so it STAYS live and lies; the owner sweep
    //     then over-releases the consumer's surviving ref → the pack is freed
    //     while Q still holds it. Reproduced here to prove the mechanism (and to
    //     document precisely what the OwnerGuard prevents).
    {
        xi_pack_handle P = XI_PACK_NULL;
        { xi::ImagePool::OwnerGuard g(X);
          xi_pack_builder b = fi->builder_new();
          fi->builder_add_i64(b, "seq", 9);
          P = fi->builder_seal(b); }
        CHECK(P != XI_PACK_NULL);
        { xi::ImagePool::OwnerGuard g(Q); fi->retain(P); }   // consumer co-holds (rc 2)

        reg.release_as(P, 0);                        // UNGUARDED release (owner 0 != creator X): rc 1
        CHECK(reg.owner_refs(X) == 1);               // STRANDED tag — the defect
        CHECK(xi::ImagePool::sweep_packs_for(X) == 1);       // sweep over-releases Q's live ref
        int64_t v = 0;
        CHECK(fi->get_i64(P, "seq", &v) == 0);       // P freed under Q — the finding-② UAF
        // Q's ref is now dangling by construction; the over-release already drove
        // rc to 0, so nothing more to release — the table is back to baseline.
        CHECK(reg.live_frames() == base_frames);
    }
}

int main() {
    std::printf("[test] xi.pack@1 carved data-plane door + dispatch dual-carry\n");
    test_door_probe();
    test_build_read_roundtrip();
    test_refcount_lifecycle();
    test_dispatch_dual_carry();
    test_owner_sweep_regression();
    test_has_source_pack_identity();
    test_bin_pool_exhaustion_no_null_span();
    test_cross_plane_owner_sweep_keeps_coowned_pack_buffers();
    test_pack_v4_door();
    test_add_mp_seam_canonicalize();
    test_reinit_creator_tag_ownerguard();
    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
