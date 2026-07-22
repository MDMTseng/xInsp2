// test_mp_fixtures.cpp — generate + verify the CANONICAL msgpack golden fixtures.
//
// The canonical profile (xi/xi_mp.hpp) is implemented independently in C++,
// TypeScript, and Python from the SAME spec
// (docs/new_gen/07-uniform-keyed-buffer-plane.md, decisions pinned in
// contract/canonical-profile-notes.md). These goldens are how the three are
// held in lockstep: every *.bin here is produced by the C++ Writer/canonicalizer
// and byte-compared on every run; the sibling TS/Py codecs decode+re-encode the
// SAME files and must reproduce the SAME bytes. Two independent implementations
// validating one spec.
//
// Three fixture categories (all under protocol/fixtures/canonical/):
//   * canonical  — well-formed values the Writer emits; verify re-encodes and
//                  byte-compares. Covers every scalar at its width boundaries
//                  (proving max-width NEVER compacts), str/bin length
//                  boundaries, and nested containers.
//   * compact    — a foreign COMPACT input + its canonical re-encoding; verify
//                  that canonicalize(compact) == the canonical golden (proves
//                  the re-encoder / ingress core).
//   * hostile    — malformed / adversarial bytes; verify the validating reader
//                  REJECTS each with the expected Status (truncation, depth
//                  bomb, trailing bytes, unexpected ext, reserved byte).
//
// Modes:
//   test_mp_fixtures            (ctest default) — VERIFY every fixture.
//   test_mp_fixtures --regen    REGENERATE the .bin files + manifest.json.
//
// Regeneration (from repo root, after a Release build):
//   ctest --test-dir backend/build -R mp_fixtures                    # verify
//   ./backend/build/Release/test_mp_fixtures.exe --regen             # rewrite
//
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <xi/xi_json.hpp>
#include <xi/xi_mp.hpp>

using xi::mp::Bytes;
using xi::mp::Reader;
using xi::mp::Status;
using xi::mp::Writer;

namespace {

std::string b64(const Bytes& in) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t* p = in.data();
    size_t n = in.size();
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t b = (uint32_t)p[i] << 16;
        if (i + 1 < n) b |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < n) b |= (uint32_t)p[i + 2];
        o.push_back(T[(b >> 18) & 63]);
        o.push_back(T[(b >> 12) & 63]);
        o.push_back(i + 1 < n ? T[(b >> 6) & 63] : '=');
        o.push_back(i + 2 < n ? T[b & 63] : '=');
    }
    return o;
}

std::string repeat(char c, size_t n) { return std::string(n, c); }

// ---- canonical fixtures (well-formed; the Writer is the source of truth) -----

struct Canon {
    std::string           name;
    std::string           content;  // human description so the sibling can reproduce
    std::function<Bytes()> build;
};

Bytes int_boundaries() {
    // Every signed boundary + the two values that require uint64. Each element
    // is 9 bytes: proving the width NEVER compacts. The last two are 0xcf.
    Writer w;
    const int64_t si[] = {
        0, 1, -1, 127, 128, 255, 256, -128, -129,
        32767, -32768, 65535, 65536,
        2147483647LL, -2147483648LL, 4294967295LL, 4294967296LL,
        INT64_MAX, INT64_MIN,
    };
    const uint64_t su[] = {(uint64_t)INT64_MAX + 1, UINT64_MAX};
    w.array((uint32_t)(sizeof si / sizeof si[0] + sizeof su / sizeof su[0]));
    for (int64_t v : si) w.int_(v);
    for (uint64_t v : su) w.uint_(v);
    return w.take();
}

Bytes float_values() {
    Writer w;
    double nan = std::nan("");
    double inf = 1.0;  // placeholder; set below via bit ops for portability
    // Build +Inf / -Inf without relying on <limits> macros in the manifest text.
    uint64_t inf_bits = 0x7FF0000000000000ull;
    std::memcpy(&inf, &inf_bits, sizeof inf);
    w.array(7);
    w.float_(0.0);
    w.float_(-0.0);
    w.float_(1.5);
    w.float_(-2.25);
    w.float_(nan);
    w.float_(inf);
    w.float_(-inf);
    return w.take();
}

Bytes misc_scalars() {
    Writer w;
    w.array(3);
    w.nil();
    w.boolean(true);
    w.boolean(false);
    return w.take();
}

Bytes str_boundaries() {
    // Lengths straddling the compact str8/str16 boundaries; all emit str32.
    Writer w;
    w.array(5);
    w.str("");
    w.str(repeat('a', 31));
    w.str(repeat('a', 32));
    w.str(repeat('a', 255));
    w.str(repeat('a', 256));
    return w.take();
}

Bytes bin_boundaries() {
    Writer w;
    w.array(3);
    Bytes b0, b255(255, 0xAB), b256(256, 0xCD);
    w.bin(b0.data(), b0.size());
    w.bin(b255.data(), b255.size());
    w.bin(b256.data(), b256.size());
    return w.take();
}

Bytes nested_frame() {
    // A realistic v3 pack plane: a map of typed entries, one of which is an
    // image descriptor (nested map), one a list of blobs (array of maps).
    Writer w;
    w.map(3);
    w.key("frame");
    w.map(5);
    w.key("w");      w.int_(640);
    w.key("h");      w.int_(480);
    w.key("c");      w.int_(3);
    w.key("dtype");  w.str("u8");
    w.key("stride"); w.int_(1920);
    w.key("blobs");
    w.array(2);
    w.map(3); w.key("area"); w.int_(1234); w.key("cx"); w.float_(12.5); w.key("cy"); w.float_(48.0);
    w.map(3); w.key("area"); w.int_(56);   w.key("cx"); w.float_(300.0);w.key("cy"); w.float_(64.25);
    w.key("threshold"); w.int_(128);
    return w.take();
}

std::vector<Canon> canon_fixtures() {
    return {
        {"scalar_int_boundaries",
         "array(21) of int at every signed boundary + INT64_MAX+1 and UINT64_MAX "
         "(the last two encode uint64 0xcf; all others int64 0xd3; every element 9 bytes)",
         int_boundaries},
        {"scalar_float",
         "array(7) float64: 0.0, -0.0, 1.5, -2.25, NaN, +Inf, -Inf",
         float_values},
        {"scalar_misc",
         "array(3): nil, true, false",
         misc_scalars},
        {"str_boundaries",
         "array(5) str of length 0, 31, 32, 255, 256 (all str32)",
         str_boundaries},
        {"bin_boundaries",
         "array(3) bin of length 0, 255, 256 (all bin32)",
         bin_boundaries},
        {"nested_frame",
         "map(3){ frame: map(5){w,h,c,dtype,stride}, blobs: array(2) of map(3), threshold: int }",
         nested_frame},
    };
}

// ---- compact -> canonical pair (proves the re-encoder) -----------------------

struct CompactPair {
    std::string name;     // <name>_compact.bin + <name>_canonical.bin
    std::string content;
    Bytes       compact;  // hand-built foreign compact input
    Bytes       canonical;
};

CompactPair make_compact_pair() {
    // Foreign COMPACT input, hand-built at minimum widths the Writer never emits:
    //   fixmap(4){ "n": fixint 5, "big": uint16 1000, "pi": float32 3.5,
    //              "tags": fixarray(2)[ fixstr"a", fixstr"bb" ] }.
    Bytes compact = {
        0x84,                                            // fixmap(4)
        0xA1, 'n',              0x05,                     // "n": fixint 5
        0xA3, 'b','i','g',      0xCD, 0x03, 0xE8,         // "big": uint16 1000
        0xA2, 'p','i',          0xCA, 0x40, 0x60, 0x00, 0x00,   // "pi": float32 3.5
        0xA4, 't','a','g','s',  0x92, 0xA1, 'a', 0xA2, 'b','b', // "tags": [ "a", "bb" ]
    };

    // The canonical re-encoding, built with the Writer in the SAME key order.
    Writer w;
    w.map(4);
    w.key("n");    w.int_(5);
    w.key("big");  w.int_(1000);
    w.key("pi");   w.float_(3.5);
    w.key("tags"); w.array(2); w.str("a"); w.str("bb");

    return {"reencode", "compact fixmap(4){n,big,pi(float32),tags} -> canonical", compact, w.take()};
}

// ---- hostile fixtures (rejected with a specific Status) ----------------------

struct Hostile {
    std::string name;
    std::string content;
    Bytes       bytes;
    Status      expect;
};

std::vector<Hostile> hostile_fixtures() {
    std::vector<Hostile> hs;
    hs.push_back({"truncated_str", "str8 declares 200 bytes, 3 present",
                  {0xD9, 200, 'a', 'b', 'c'}, Status::LengthOverflow});
    hs.push_back({"truncated_int", "int64 tag, 2 of 8 payload bytes",
                  {0xD3, 0x00, 0x00}, Status::Truncated});
    {
        Bytes b;
        for (int i = 0; i < 200; ++i) b.push_back(0x91);  // 200 nested fixarray(1)
        b.push_back(0xC0);                                // nil
        hs.push_back({"depth_bomb", "200 nested fixarray(1) exceeds default max_depth 64",
                      std::move(b), Status::DepthExceeded});
    }
    hs.push_back({"trailing_bytes", "a complete nil followed by a stray byte",
                  {0xC0, 0xC0}, Status::TrailingBytes});
    hs.push_back({"reserved_byte", "the never-used byte 0xc1",
                  {0xC1}, Status::ReservedByte});
    hs.push_back({"unexpected_ext", "fixext1 with default reject policy",
                  {0xD4, 0x07, 0xAA}, Status::UnexpectedExt});
    hs.push_back({"map_missing_value", "map32(1) with only the key present",
                  {0xDF, 0, 0, 0, 1, 0xA1, 'k'}, Status::Truncated});
    // Ruling 2 (2026-07-02): the pack plane is string-keyed. A foreign map with
    // a non-string key is rejected at the boundary (Python's MapKeyError twin).
    hs.push_back({"nonstring_key", "fixmap(1) with an integer key {1: 2}",
                  {0x81, 0x01, 0x02}, Status::NonStringKey});
    return hs;
}

// ---- io ----------------------------------------------------------------------

bool read_file(const std::string& path, Bytes& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool write_file(const std::string& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    return (bool)out;
}

std::string canonical_dir() {
    const char* fx = std::getenv("XINSP2_FIXTURES");
    std::string base = fx ? fx : "../../protocol/fixtures";
    return base + "/canonical";
}

// ---- regen -------------------------------------------------------------------

int regen(const std::string& dir) {
    xi::Json manifest = xi::Json::object();
    manifest.set("note",
        "Canonical msgpack golden fixtures. Generated by "
        "backend/tests/test_mp_fixtures.cpp --regen from the canonical codec "
        "(backend/include/xi/xi_mp.hpp). Do not edit by hand. The sibling TS/Python "
        "codecs decode + re-encode these SAME .bin files and must reproduce the "
        "same bytes. Profile decisions: contract/canonical-profile-notes.md.");

    xi::Json canon = xi::Json::array();
    for (const auto& f : canon_fixtures()) {
        Bytes bytes = f.build();
        std::string path = dir + "/" + f.name + ".bin";
        if (!write_file(path, bytes)) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
        std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
        xi::Json j = xi::Json::object();
        j.set("file", f.name + ".bin");
        j.set("category", "canonical");
        j.set("size", (int64_t)bytes.size());
        j.set("content", f.content);
        j.set("b64", b64(bytes));
        canon.push(j);
    }
    manifest.set("canonical", canon);

    {
        CompactPair cp = make_compact_pair();
        std::string cpath = dir + "/" + cp.name + "_compact.bin";
        std::string kpath = dir + "/" + cp.name + "_canonical.bin";
        if (!write_file(cpath, cp.compact) || !write_file(kpath, cp.canonical)) {
            std::fprintf(stderr, "cannot write compact pair\n"); return 1;
        }
        std::printf("wrote %s (%zu) + %s (%zu)\n",
                    cpath.c_str(), cp.compact.size(), kpath.c_str(), cp.canonical.size());
        xi::Json j = xi::Json::object();
        j.set("name", cp.name);
        j.set("compact_file", cp.name + "_compact.bin");
        j.set("canonical_file", cp.name + "_canonical.bin");
        j.set("content", cp.content);
        j.set("compact_b64", b64(cp.compact));
        j.set("canonical_b64", b64(cp.canonical));
        manifest.set("compact_pair", j);
    }

    xi::Json hostile = xi::Json::array();
    for (const auto& h : hostile_fixtures()) {
        std::string path = dir + "/hostile_" + h.name + ".bin";
        if (!write_file(path, h.bytes)) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
        std::printf("wrote %s (%zu bytes)\n", path.c_str(), h.bytes.size());
        xi::Json j = xi::Json::object();
        j.set("file", "hostile_" + h.name + ".bin");
        j.set("category", "hostile");
        j.set("size", (int64_t)h.bytes.size());
        j.set("content", h.content);
        j.set("expected_status", xi::mp::status_str(h.expect));
        j.set("b64", b64(h.bytes));
        hostile.push(j);
    }
    manifest.set("hostile", hostile);

    std::string mpath = dir + "/manifest.json";
    std::ofstream mo(mpath, std::ios::binary);
    if (!mo) { std::fprintf(stderr, "cannot write %s\n", mpath.c_str()); return 1; }
    std::string text = manifest.dump_pretty();
    mo.write(text.data(), (std::streamsize)text.size());
    mo.put('\n');
    std::printf("wrote %s\n", mpath.c_str());
    return 0;
}

// ---- verify ------------------------------------------------------------------

int verify(const std::string& dir) {
    int failures = 0;

    // canonical: the Writer must reproduce the golden bytes exactly.
    for (const auto& f : canon_fixtures()) {
        Bytes want, got = f.build();
        std::string path = dir + "/" + f.name + ".bin";
        if (!read_file(path, want)) {
            std::fprintf(stderr, "FAIL %s: missing golden (run --regen)\n", f.name.c_str());
            ++failures; continue;
        }
        if (got != want) {
            std::fprintf(stderr, "FAIL %s: encoder (%zu B) != golden (%zu B)\n",
                         f.name.c_str(), got.size(), want.size());
            ++failures; continue;
        }
        // And the encoder's own output must validate + canonicalize to itself.
        Writer round;
        if (Reader(got).canonicalize(round) != Status::Ok || round.bytes() != got) {
            std::fprintf(stderr, "FAIL %s: not idempotent under canonicalize\n", f.name.c_str());
            ++failures; continue;
        }
        std::printf("[mp] ok canonical %-24s %zu bytes\n", f.name.c_str(), got.size());
    }

    // compact pair: canonicalize(compact_golden) must equal the canonical golden.
    {
        CompactPair cp = make_compact_pair();
        Bytes compact_g, canon_g;
        bool ok = read_file(dir + "/" + cp.name + "_compact.bin", compact_g) &&
                  read_file(dir + "/" + cp.name + "_canonical.bin", canon_g);
        if (!ok) {
            std::fprintf(stderr, "FAIL compact pair: missing golden (run --regen)\n");
            ++failures;
        } else if (compact_g != cp.compact || canon_g != cp.canonical) {
            std::fprintf(stderr, "FAIL compact pair: golden bytes drifted from generator\n");
            ++failures;
        } else {
            Writer out;
            Status s = Reader(compact_g).canonicalize(out);
            if (s != Status::Ok || out.bytes() != canon_g) {
                std::fprintf(stderr, "FAIL compact pair: canonicalize(compact) != canonical (%s)\n",
                             xi::mp::status_str(s));
                ++failures;
            } else {
                std::printf("[mp] ok compact   %-24s %zu -> %zu bytes\n",
                            cp.name.c_str(), compact_g.size(), canon_g.size());
            }
        }
    }

    // hostile: the validating reader must reject each with the expected Status.
    for (const auto& h : hostile_fixtures()) {
        Bytes bytes;
        std::string path = dir + "/hostile_" + h.name + ".bin";
        if (!read_file(path, bytes)) {
            std::fprintf(stderr, "FAIL hostile_%s: missing golden (run --regen)\n", h.name.c_str());
            ++failures; continue;
        }
        if (bytes != h.bytes) {
            std::fprintf(stderr, "FAIL hostile_%s: golden bytes drifted from generator\n", h.name.c_str());
            ++failures; continue;
        }
        Status s = xi::mp::validate(bytes.data(), bytes.size());
        if (s != h.expect) {
            std::fprintf(stderr, "FAIL hostile_%s: got %s, expected %s\n",
                         h.name.c_str(), xi::mp::status_str(s), xi::mp::status_str(h.expect));
            ++failures; continue;
        }
        std::printf("[mp] ok hostile   %-24s rejected: %s\n", h.name.c_str(), xi::mp::status_str(s));
    }

    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    const bool do_regen = (argc > 1 && std::string(argv[1]) == "--regen");
    const std::string dir = canonical_dir();
    if (do_regen) return regen(dir);

    int failures = verify(dir);
    if (failures == 0) {
        std::printf("\nALL CANONICAL MSGPACK FIXTURE GOLDENS MATCH\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d CANONICAL MSGPACK FIXTURE MISMATCH(ES)\n", failures);
    return 1;
}
