//
// xex1_v2_identity_test.cpp — the polaris2 wave-2 HEADLINE: memory ≈ wire ≈ disk
// (docs/new_gen/07-uniform-keyed-buffer-plane.md, "the preview frame, the
// record/replay file, and the in-memory plane become the same bytes").
//
// NAMING NOTE: the file/test name keeps its pilot-era "v2" spelling (doc 10's
// historical-spelling rule); the frame under test is XEX1-v3 — the FINALIZED
// canonical dump, i.e. the v2 draft + per-entry tags ([tag, value] entries,
// gate P3). The identity claims are unchanged: the tag rides BESIDE the value,
// so every entry's memory-plane bytes still appear verbatim on the wire.
//
// This is the doc-07 claim made an EXECUTABLE ASSERTION. It builds a real
// xi::Pack (the host-side v3 container), seals it, dumps it to an XEX1-v3 frame
// (the canonical frame dump), and proves three equalities on the small plane:
//
//   1. memory  == canonical : each entry's stored arena bytes ARE already
//      canonical msgpack — re-encoding the same value through xi::mp::Writer
//      yields byte-identical bytes. The pack does not hold a private layout it
//      must transcode; it holds the wire encoding.
//   2. wire     == memory   : the XEX1-v3 dump splices those arena bytes onto the
//      wire VERBATIM (Writer::raw_canonical), so each entry's memory bytes appear
//      as a contiguous slice of the emitted pack. Boundaries are copies, not
//      transformations.
//   3. disk     == wire      : the emitted frame written to a file and read back
//      is byte-identical (a record_save-style dump is a byte copy, not a
//      re-serialization).
//
// Images ride the large plane: pixels are pooled and inlined as `bin` on export
// (doc 07 D1/D2), and we assert the inlined pixels equal the pool buffer exactly.
//
#include <xi/xi_pack.hpp>       // xi::Pack / xi::PackBuilder (the container)
#include <xi/xi_image_pool.hpp>  // ImagePool (pooled image pixels) + leak oracle
#include <xi/xi_mp.hpp>          // xi::mp::Writer/Reader (the canonical codec)

#include "xex1_encode.hpp"       // xi::xex1::V3Entry / encode_frame_v3

#include <algorithm>
#include <cstdint>
#include <cstdio>
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

// Re-encode a pack entry's typed value through xi::mp::Writer, returning the
// canonical bytes an independent encoder produces for the same logical value.
static std::vector<uint8_t> reencode(const xi::Pack& f, std::string_view key, xi::PackTag tag) {
    xi::mp::Writer w;
    switch (tag) {
        case xi::PackTag::I64: w.int_(*f.get_i64(key)); break;
        case xi::PackTag::F64: w.float_(*f.get_f64(key)); break;
        case xi::PackTag::Str: w.str(*f.get_str(key)); break;
        case xi::PackTag::Mp:  { auto s = *f.get_mp(key); w.raw_canonical(s.data(), s.size()); break; }
        case xi::PackTag::Bin: { auto s = *f.get_bin(key); w.bin(s.data(), s.size()); break; }
        default: break;   // Image has no small-plane scalar form
    }
    return w.take();
}

int main() {
    std::printf("[test] XEX1-v3 memory == wire == disk identity\n");
    const int base_live = xi::ImagePool::instance().cumulative().live_now;

    // ---- build a real pack: scalars + str + nested msgpack + a pooled image --
    std::vector<uint8_t> pixels = {10, 20, 30, 40};   // a 2x2x1 gray image

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
        b.add_str("label", "ok");
        b.add_mp("blobs", blobs_mp.data(), blobs_mp.size());
        b.add_image("frame", 2, 2, 1, pixels.data());
        pack = b.seal();
    }
    CHECK(pack.size() == 6);

    // ---- host-side generic dump: read arena bytes VERBATIM into V3Entry --------
    // (the same walk expose does across the door, but here in-process we can read
    // the arena bytes directly and prove they flow to the wire unchanged.)
    std::vector<xi::xex1::V3Entry> entries;
    std::vector<std::vector<uint8_t>> memory_planes;   // per-entry arena bytes (non-image)
    uint64_t seq = 0;
    for (size_t i = 0; i < pack.size(); ++i) {
        std::string_view key = pack.key_at(i);
        xi::PackTag tag = pack.tag_at(i);
        if (key == "$seq") { seq = (uint64_t)*pack.get_i64(key); continue; }  // lift, don't dump

        xi::xex1::V3Entry e;
        e.key = std::string(key);
        e.tag = (uint8_t)tag;
        if (tag == xi::PackTag::Image) {
            auto im = *pack.get_image(key);
            e.w = im.width; e.h = im.height; e.c = im.channels;
            e.px = im.pixels.data(); e.px_len = im.pixels.size();
        } else {
            std::span<const uint8_t> raw = pack.raw_at(i);   // the memory plane
            std::vector<uint8_t> mem(raw.begin(), raw.end());

            // (1) memory == canonical: the arena bytes ARE canonical msgpack.
            std::vector<uint8_t> canon = reencode(pack, key, tag);
            CHECK(mem == canon);

            e.value = mem;                 // splice the arena bytes onto the wire
            memory_planes.push_back(mem);
        }
        entries.push_back(std::move(e));
    }

    std::vector<uint8_t> wire = xi::xex1::encode_frame_v3("cam0", seq, entries);

    SECTION("wire is a well-formed XEX1 frame carrying canonical msgpack");
    CHECK(wire.size() > 4);
    CHECK(wire[0] == 'X' && wire[1] == 'E' && wire[2] == 'X' && wire[3] == '1');
    CHECK(xi::mp::validate(wire.data() + 4, wire.size() - 4) == xi::mp::Status::Ok);

    SECTION("(2) each entry's memory plane appears VERBATIM in the wire bytes");
    for (const auto& mem : memory_planes)
        CHECK(contains(wire, mem));
    // The pooled image pixels are inlined as bin and appear verbatim too.
    CHECK(contains(wire, pixels));

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
