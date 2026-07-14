//
// xex1_v2_identity_test.cpp — the XEX1-v3 CANONICAL-WALK identity:
//                             canonical_value == encoder wire == disk.
//
// NAMING NOTE: the file/test name keeps its pilot-era "v2" spelling (doc 10's
// historical-spelling rule); the frame under test is XEX1-v3 — the FINALIZED
// canonical dump ([tag, value] entries, gate P3).
//
// MEMORY == WIRE, RESTORED (④A, doc 28 finding ④): the slab container stores
// each INLINE entry's payload as its CANONICAL MSGPACK VALUE — i64=int64
// 0xd3+8, f64=float64 0xcb+8 (NaN flattened at add), bool=0xc2/0xc3, str=str32,
// small bin=bin32, nested Mp verbatim. So raw_at(i) IS the wire bytes again,
// and the identity is STRUCTURAL: canonical_value(i) is a verbatim SPLICE of
// raw_at(i), not a re-encode a walker must keep honest. (This reverses the
// interim slab state where scalars lived raw and memory != wire.)
//
// THE INVARIANT THIS FILE PINS, executably:
//
//   Pack::raw_at(i)  ==  Pack::canonical_value(i, w)              (inline: structural)
//                    ==  independent xi::mp::Writer re-encode
//                    ==  the bytes the XEX1-v3 encoder emits
//                    ==  the bytes on disk.
//
// The XEX1-v3 encoder (xex1_encode.hpp — the same encoder the protocol/fixtures
// goldens pin, so walk == encoder here transitively means walk == goldens)
// splices those bytes verbatim, and disk is a byte copy of the wire.
//
// The test's value is unchanged — it caught encode drift before and must
// still: it FAILS if the canonical walk and the encoder ever diverge. It
// proves, on a real pack:
//
//   0. the restored identity: for every inline entry raw_at(i) EQUALS the
//      canonical bytes byte-for-byte (i64/f64 carry the 0xd3/0xcb marker, bool
//      is 0xc2/0xc3, str is str32, Mp is its canonical value) — memory == wire
//      is a structure, not a convention the walker re-derives.
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

    // ---- (0) the restored identity, made executable: raw_at == canonical -----
    // memory == wire (④A): raw_at(i) IS the canonical value for every inline
    // entry. i64/f64 carry the 0xd3/0xcb marker + 8 bytes, bool is one 0xc2/
    // 0xc3 byte, str is a str32 value, Mp is its canonical bytes. Each must
    // equal the independent re-encode byte-for-byte. If raw_at ever DIVERGES
    // from canonical again, someone reintroduced a raw-scalar representation and
    // this file's premise (and comments) must be revisited.
    SECTION("(0) memory == wire: inline raw_at EQUALS the canonical msgpack value");
    pack.for_each_entry([&](const xi::Pack::EntryView& e) {
        if (e.external) return;                             // image: no inline bytes
        std::vector<uint8_t> mem(e.raw.begin(), e.raw.end());
        std::vector<uint8_t> canon = reencode(pack, e.key, e.tag);
        CHECK(mem == canon);                                // the structural identity
        switch (e.tag) {
            case xi::PackTag::I64:
            case xi::PackTag::F64:
                CHECK(mem.size() == 9 && (mem[0] == 0xd3 || mem[0] == 0xcb));
                break;
            case xi::PackTag::Bool:
                CHECK(mem.size() == 1 && (mem[0] == 0xc2 || mem[0] == 0xc3));
                break;
            case xi::PackTag::Str:
                CHECK(mem.size() >= 5 && mem[0] == 0xdb);   // str32 header + bytes
                break;
            case xi::PackTag::Mp:
                CHECK(mem == canon);                        // nested canonical verbatim
                break;
            default: break;
        }
    });

    // ---- (1) walk == codec, and the frame entries come FROM the walk --------
    // This is the port shape every serialization site uses: for_each_entry +
    // canonical_value produce the wire bytes. Since ④A canonical_value for an
    // inline entry is a verbatim splice of raw_at, so walk == raw_at == codec.
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
