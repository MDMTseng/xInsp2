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
//   4. Dispatch DUAL-CARRY — TriggerBus::emit_pack carries a Pack on the SAME
//      bus that TriggerBus::emit carries a Record, and neither bleeds into the
//      other (a pack event has pack!=NULL/images empty; a record event has
//      images!=empty/pack==NULL). Pack + image refs balance to baseline.
//
// The plugin-side door (blob_analysis's pack-in/pack-out) + the real end-to-end
// mock_camera->blob_analysis flow are exercised in the PLUGIN test
// (plugins/.../pack_pilot_test) against the actually-built DLLs.
//
#include <xi/xi_pack_abi.hpp>    // PackRegistry, pack_v1_iface, install_pack_abi
#include <xi/xi_image_pool.hpp>   // ImagePool::make_host_api / cumulative().live_now
#include <xi/xi_trigger_bus.hpp>  // TriggerBus / install_trigger_hook
#include <xi/xi_mp.hpp>           // decode the nested-msgpack round-trip

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

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
    CHECK(fi->count(f) == 6);
    int32_t klen = 0;
    const char* k0 = fi->key_at(f, 0, &klen);
    CHECK(k0 && klen == 9 && std::string(k0, (size_t)klen) == "threshold");
    CHECK(fi->tag_at(f, 0) == XI_PACK_TAG_I64);
    CHECK(fi->tag_at(f, 4) == XI_PACK_TAG_IMAGE);
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
// (4) Dispatch dual-carry: Pack and Record on the SAME bus, no crosstalk.
// ---------------------------------------------------------------------------
static void test_dispatch_dual_carry() {
    SECTION("TriggerBus carries a Pack (emit_pack) alongside a Record (emit)");
    xi::install_pack_abi();
    xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_trigger_hook(host);        // wires the Record emit path (api.emit_record)
    const xi_pack_v1* fi = xi::pack_v1_iface();

    int base = pool_live();

    // A sink that captures every dispatched event (the inspection worker stand-in).
    std::vector<xi::TriggerEvent> got;
    xi::TriggerBus::instance().set_sink([&](xi::TriggerEvent ev) { got.push_back(std::move(ev)); });

    // --- PACK currency: build a pack with an image + a scalar, emit it. ---
    std::vector<uint8_t> gray(16, 0); gray[0] = 255;
    xi_pack_builder b = fi->builder_new();
    fi->builder_add_i64(b, "seq", 11);
    fi->builder_add_image(b, "frame", 4, 4, 1, gray.data());
    xi_pack_handle f = fi->builder_seal(b);        // emitter's ref (rc 1)
    fi->emit_pack("cam_frame", XI_TRIGGER_NULL, f, 0);  // event takes a 2nd ref
    fi->release(f);                                 // drop the emitter's ref (rc 1, held by event)

    // --- RECORD currency: mint an image handle and emit it the classic way. ---
    xi_image_handle rimg = xi::ImagePool::instance().create(4, 4, 1);   // my ref (rc 1)
    xi_record_image rentry{ "frame", rimg };
    xi::TriggerBus::instance().emit("cam_record", XI_TRIGGER_NULL, 0, &rentry, 1, nullptr); // bus addrefs
    xi::ImagePool::instance().release(rimg);         // drop my ref (event holds one)

    CHECK(got.size() == 2);

    // The pack event: pack set, images empty; the pack reads back correctly.
    const xi::TriggerEvent* fe = nullptr;
    const xi::TriggerEvent* re = nullptr;
    for (auto& ev : got) {
        if (ev.pack != XI_PACK_NULL) fe = &ev; else re = &ev;
    }
    CHECK(fe != nullptr);
    CHECK(re != nullptr);
    if (fe) {
        CHECK(fe->leader_source == "cam_frame");
        CHECK(fe->images.empty());                   // a pack event carries no image map
        int64_t seq = 0;
        CHECK(fi->get_i64(fe->pack, "seq", &seq) == 1 && seq == 11);
        xi_pack_image iv{};
        CHECK(fi->get_image(fe->pack, "frame", &iv) == 1 && iv.width == 4);
    }
    // The record event: images set, pack NULL (the Record path is untouched).
    if (re) {
        CHECK(re->leader_source == "cam_record");
        CHECK(re->pack == XI_PACK_NULL);           // no pack bled into the Record path
        CHECK(re->images.size() == 1);
    }

    // Release both events exactly as the dispatcher's release_trigger_event_ does.
    for (auto& ev : got) {
        for (auto& [s, h] : ev.images) xi::ImagePool::instance().release(h);
        if (ev.pack != XI_PACK_NULL) xi::TriggerBus::instance().release_pack_(ev.pack);
    }
    got.clear();
    xi::TriggerBus::instance().clear_sink();

    CHECK(pool_live() == base);   // every pack + image ref accounted for
}

int main() {
    std::printf("[test] xi.pack@1 carved data-plane door + dispatch dual-carry\n");
    test_door_probe();
    test_build_read_roundtrip();
    test_refcount_lifecycle();
    test_dispatch_dual_carry();
    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
