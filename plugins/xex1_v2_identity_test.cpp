//
// xex1_v2_identity_test.cpp — the XEX1-v3 CANONICAL-WALK identity:
//                             canonical_value == encoder wire == disk.
//
// NAMING NOTE: the file/test name keeps its pilot-era "v2" spelling (doc 10's
// historical-spelling rule); the frame under test is XEX1-v3 — the FINALIZED
// canonical dump ([tag, value] entries, gate P3).
//
// WHY THE OLD IDENTITY DIED (pack-v3 slab migration, packv3 branch d8fe140):
// this test used to be the polaris2 wave-2 headline "memory ≈ wire ≈ disk" —
// it spliced pack.raw_at(i) onto the wire and asserted the entry's IN-MEMORY
// arena bytes were already canonical msgpack, appearing VERBATIM in the frame.
// That premise was retired BY DESIGN: the slab container stores scalars RAW
// (i64/f64 = 8 aligned bytes, bool = one 0/1 byte, str = raw bytes with the
// length in the DirEntry) so a typed read is one load with zero msgpack
// decode. Memory != wire for scalars now; raw_at(i) returns the raw stored
// payload, not a canonical value.
//
// THE INVARIANT THAT HOLDS NOW (what this file pins, executably):
//
//   Pack::canonical_value(i, w)  ==  independent xi::mp::Writer re-encode
//                                ==  the bytes the XEX1-v3 encoder emits
//                                ==  the bytes on disk.
//
// Canonical msgpack is produced ON DEMAND at the serialization edge by the
// pack's walk API (for_each_entry + canonical_value), byte-identical to what
// the old arena stored (max-width profile, ruling-1 NaN flatten — the emit
// goes through xi::mp::Writer, the one canonical-encode truth). The XEX1-v3
// encoder (xex1_encode.hpp — the same encoder the protocol/fixtures goldens
// pin, so walk == encoder here transitively means walk == goldens) splices
// those bytes verbatim, and disk is a byte copy of the wire.
//
// The test's value is unchanged — it caught encode drift before and must
// still: it FAILS if the canonical walk and the encoder ever diverge. It
// proves, on a real pack:
//
//   0. the retirement itself: scalar raw_at bytes are RAW (8B/1B payloads),
//      NOT the canonical encoding — the old memory==wire claim is dead code,
//      not silently still true by accident. (Mp entries alone stay canonical
//      verbatim in the slab.)
//   1. walk == codec: canonical_value(i) is byte-identical to re-encoding the
//      same typed value through an independent xi::mp::Writer.
//   2. walk == wire: the XEX1-v3 frame built from the canonical walk carries
//      each entry's canonical bytes as a contiguous verbatim slice, AND is
//      byte-identical to a frame built from the independent re-encodes — the
//      executable trap for walk-vs-encoder divergence.
//   3. wire == disk: the emitted frame written to a file and read back is
//      byte-identical (a record_save-style dump is a byte copy).
//
// Images ride the large plane exactly as before: pixels are pooled and inlined
// as `bin` on export (doc 07 D1/D2), and we assert the inlined pixels equal
// the pool buffer exactly.
//
#include <xi/xi_pack.hpp>       // xi::Pack / xi::PackBuilder (the slab container)
#include <xi/xi_image_pool.hpp>  // ImagePool (pooled image pixels) + leak oracle
#include <xi/xi_mp.hpp>          // xi::mp::Writer/Reader (the canonical codec)

#include "xex1_encode.hpp"       // xi::xex1::V3Entry / encode_frame_v3

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
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

// Does `hay` contain `needle` as a contiguous byte run?
static bool contains(const std::vector<uint8_t>& hay, const std::vector<uint8_t>& needle) {
    if (needle.empty()) return true;
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

// Re-encode a pack entry's typed value through an INDEPENDENT xi::mp::Writer —
// the canonical bytes the codec produces for the same logical value, read back
// through the typed accessors (not the walk). This is the drift oracle the
// canonical walk is compared against.
static std::vector<uint8_t> reencode(const xi::Pack& f, std::string_view key, xi::PackTag tag) {
    xi::mp::Writer w;
    switch (tag) {
        case xi::PackTag::I64:  w.int_(*f.get_i64(key)); break;
        case xi::PackTag::F64:  w.float_(*f.get_f64(key)); break;
        case xi::PackTag::Bool: w.boolean(*f.get_bool(key)); break;
        case xi::PackTag::Str:  w.str(*f.get_str(key)); break;
        case xi::PackTag::Mp:  { auto s = *f.get_mp(key); w.raw_canonical(s.data(), s.size()); break; }
        case xi::PackTag::Bin: { auto s = *f.get_bin(key); w.bin(s.data(), s.size()); break; }
        default: break;   // Image/Tensor have no single canonical scalar form
    }
    return w.take();
}

int main() {
    std::printf("[test] XEX1-v3 canonical-walk identity: canonical_value == wire == disk\n");
    const int base_live = xi::ImagePool::instance().cumulative().live_now;

    // ---- build a real pack: scalars + str + nested msgpack + a pooled image --
    std::vector<uint8_t> pixels = {10, 20, 30, 40};   // a 2x2x1 gray image

    // A payload NaN — must reach the wire as the one flattened quiet pattern
    // (ruling 1), whichever path (walk or independent re-encode) emits it.
    const double nan_payload = [] {
        uint64_t bits = 0xfff800000000beefull;
        double d; std::memcpy(&d, &bits, sizeof d); return d;
    }();

    // A nested "blobs" entry, exactly as blob_analysis would build it.
    xi::mp::Writer blobs;
    blobs.array(2);
    blobs.map(2); blobs.key("area"); blobs.int_(100); blobs.key("cx"); blobs.float_(2.5);
    blobs.map(2); blobs.key("area"); blobs.int_(250); blobs.key("cx"); blobs.float_(8.0);
    std::vector<uint8_t> blobs_mp = blobs.bytes();

    xi::Pack pack;
    {
        xi::PackBuilder b;
        b.add_i64("$seq", 7);            // reserved ordering key (lifted to wire top)
        b.add_i64("count", 42);
        b.add_f64("score", 1.5);
        b.add_f64("nanval", nan_payload);
        b.add_bool("pass", true);
        b.add_str("label", "ok");
        b.add_mp("blobs", blobs_mp.data(), blobs_mp.size());
        b.add_image("frame", 2, 2, 1, pixels.data());
        pack = b.seal();
    }
    CHECK(pack.size() == 8);

    // ---- (0) the retirement, made executable: slab scalars are RAW ----------
    // raw_at(i) is the raw stored payload now — 8 bytes for i64/f64 (no 0xd3/
    // 0xcb marker), 1 byte for bool, bare string bytes for str. If any of these
    // ever equals the canonical encoding again, someone resurrected the old
    // representation and this file's premise (and comments) must be revisited.
    SECTION("(0) memory != wire: scalar raw_at is the RAW payload, not canonical msgpack");
    pack.for_each_entry([&](const xi::Pack::EntryView& e) {
        if (e.external) return;                             // image: no inline bytes
        std::vector<uint8_t> mem(e.raw.begin(), e.raw.end());
        std::vector<uint8_t> canon = reencode(pack, e.key, e.tag);
        switch (e.tag) {
            case xi::PackTag::I64:
            case xi::PackTag::F64:
                CHECK(mem.size() == 8);                     // raw 8-byte value
                CHECK(canon.size() == 9);                   // 0xd3/0xcb + 8
                CHECK(mem != canon);
                break;
            case xi::PackTag::Bool:
                CHECK(mem.size() == 1 && (mem[0] == 0 || mem[0] == 1));
                CHECK(mem != canon);
                break;
            case xi::PackTag::Str:
                CHECK(mem.size() == e.raw.size() && mem != canon);  // no str32 header
                break;
            case xi::PackTag::Mp:
                CHECK(mem == canon);                        // Mp alone stays verbatim
                break;
            default: break;
        }
    });

    // ---- (1) walk == codec, and the frame entries come FROM the walk --------
    // This is the port shape every serialization site uses now: for_each_entry
    // + canonical_value produce the wire bytes; nothing reads raw_at at an edge.
    SECTION("(1) canonical_value(i) == independent xi::mp::Writer re-encode");
    std::vector<xi::xex1::V3Entry> entries;        // values from the canonical walk
    std::vector<xi::xex1::V3Entry> entries_indep;  // values from the independent re-encode
    std::vector<std::vector<uint8_t>> canon_planes;   // per-entry canonical bytes (non-image)
    uint64_t seq = 0;
    pack.for_each_entry([&](const xi::Pack::EntryView& e) {
        if (e.key == "$seq") { seq = (uint64_t)*pack.get_i64(e.key); return; }  // lift, don't dump

        xi::xex1::V3Entry v;
        v.key = std::string(e.key);
        v.tag = (uint8_t)e.tag;
        if (e.tag == xi::PackTag::Image) {
            auto im = *pack.get_image(e.key);
            v.w = im.width; v.h = im.height; v.c = im.channels;
            v.px = im.pixels.data(); v.px_len = im.pixels.size();
            entries_indep.push_back(v);
        } else {
            xi::mp::Writer w;
            CHECK(pack.canonical_value(e.ordinal, w));   // the canonical walk
            std::vector<uint8_t> canon_walk = w.take();

            // The independent codec path must agree byte-for-byte.
            std::vector<uint8_t> canon_indep = reencode(pack, e.key, e.tag);
            CHECK(canon_walk == canon_indep);

            xi::xex1::V3Entry vi = v;
            vi.value = canon_indep;
            entries_indep.push_back(std::move(vi));

            v.value = canon_walk;          // the walk's bytes go on the wire
            canon_planes.push_back(v.value);
        }
        entries.push_back(std::move(v));
    });

    std::vector<uint8_t> wire = xi::xex1::encode_frame_v3("cam0", seq, entries);

    SECTION("wire is a well-formed XEX1 frame carrying canonical msgpack");
    CHECK(wire.size() > 4);
    CHECK(wire[0] == 'X' && wire[1] == 'E' && wire[2] == 'X' && wire[3] == '1');
    CHECK(xi::mp::validate(wire.data() + 4, wire.size() - 4) == xi::mp::Status::Ok);

    SECTION("(2) walk == wire: canonical bytes appear VERBATIM; walk-built == re-encode-built frame");
    for (const auto& canon : canon_planes)
        CHECK(contains(wire, canon));
    // The pooled image pixels are inlined as bin and appear verbatim too.
    CHECK(contains(wire, pixels));
    // The divergence trap: the SAME frame built from the independent re-encodes
    // must be byte-identical. If canonical_value and the encoder's expected
    // canonical profile ever drift apart, this line is what goes red. (The
    // encoder itself is pinned by the protocol/fixtures goldens, so this also
    // chains the walk to the golden bytes.)
    std::vector<uint8_t> wire_indep = xi::xex1::encode_frame_v3("cam0", seq, entries_indep);
    CHECK(wire == wire_indep);

    SECTION("(3) wire == disk: dump to a file and read it back byte-for-byte");
    {
        const std::string path = std::string("xex1_v2_identity.bin");
        { std::ofstream o(path, std::ios::binary);
          o.write(reinterpret_cast<const char*>(wire.data()), (std::streamsize)wire.size()); }
        std::vector<uint8_t> disk;
        { std::ifstream in(path, std::ios::binary);
          disk.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()); }
        CHECK(disk == wire);
        std::remove(path.c_str());
    }

    // ---- the pack owns its image handle; dropping it balances the pool -------
    pack = xi::Pack{};
    CHECK(xi::ImagePool::instance().cumulative().live_now == base_live);

    if (g_failures == 0) { std::printf("\nALL TESTS PASSED\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
