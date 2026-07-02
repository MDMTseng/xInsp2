// test_mp.cpp — unit tests for the canonical msgpack codec (xi/xi_mp.hpp).
//
// In-memory properties only (the golden-fixture cross-check lives in
// test_mp_fixtures.cpp). Three groups:
//   * writer  — every scalar/container emits exactly ONE canonical width, and
//               the writer is deterministic (same input -> byte-identical),
//               with size stability across the compact boundaries it never uses.
//   * reader  — the validating reader accepts the full standard width families,
//               canonicalize() is idempotent on canonical input and widens
//               compact foreign input, and the ext policy behaves.
//   * hostile — every malformed / adversarial input is rejected with the RIGHT
//               Status (truncation, depth bomb, trailing bytes, unexpected ext,
//               reserved byte, length overflow).
//
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <xi/xi_mp.hpp>
#include <xi/xi_test.hpp>

using xi::mp::Bytes;
using xi::mp::ExtPolicy;
using xi::mp::Reader;
using xi::mp::Status;
using xi::mp::Writer;
namespace tag = xi::mp::tag;

namespace {

Bytes bin_of(std::string_view s) {
    return Bytes(reinterpret_cast<const uint8_t*>(s.data()),
                 reinterpret_cast<const uint8_t*>(s.data()) + s.size());
}

}  // namespace

int main() {
    auto results = xi::test::run_all();
    for (auto& r : results) if (!r.passed) return 1;
    return 0;
}

// ------------------------------------------------------------------ writer ---

XI_TEST(writer_emits_canonical_widths) {
    { Writer w; w.nil();          XI_EXPECT_EQ(w.bytes().size(), (size_t)1); XI_EXPECT_EQ(w.bytes()[0], tag::Nil); }
    { Writer w; w.boolean(true);  XI_EXPECT_EQ(w.bytes()[0], tag::True); }
    { Writer w; w.boolean(false); XI_EXPECT_EQ(w.bytes()[0], tag::False); }

    // Integers: always int64 (0xd3), 9 bytes — even 0 and small values.
    { Writer w; w.int_(0);   XI_EXPECT_EQ(w.bytes().size(), (size_t)9); XI_EXPECT_EQ(w.bytes()[0], tag::Int64); }
    { Writer w; w.int_(-1);  XI_EXPECT_EQ(w.bytes()[0], tag::Int64); }
    { Writer w; w.uint_(0);  XI_EXPECT_EQ(w.bytes()[0], tag::Int64); }
    { Writer w; w.uint_(127);XI_EXPECT_EQ(w.bytes()[0], tag::Int64); }

    // uint64 ONLY strictly beyond INT64_MAX.
    { Writer w; w.uint_((uint64_t)INT64_MAX);     XI_EXPECT_EQ(w.bytes()[0], tag::Int64); }
    { Writer w; w.uint_((uint64_t)INT64_MAX + 1); XI_EXPECT_EQ(w.bytes()[0], tag::UInt64); }

    // Floats always float64 (0xcb), 9 bytes.
    { Writer w; w.float_(1.5); XI_EXPECT_EQ(w.bytes().size(), (size_t)9); XI_EXPECT_EQ(w.bytes()[0], tag::Float64); }

    // str/bin always widest (str32/bin32), 5-byte header even when empty.
    { Writer w; w.str("");   XI_EXPECT_EQ(w.bytes().size(), (size_t)5); XI_EXPECT_EQ(w.bytes()[0], tag::Str32); }
    { Writer w; w.str("hi"); XI_EXPECT_EQ(w.bytes().size(), (size_t)7); XI_EXPECT_EQ(w.bytes()[0], tag::Str32); }
    { Writer w; Bytes b = bin_of("xyz"); w.bin(b.data(), b.size());
      XI_EXPECT_EQ(w.bytes().size(), (size_t)8); XI_EXPECT_EQ(w.bytes()[0], tag::Bin32); }

    // Containers always map32/array32.
    { Writer w; w.map(0);   XI_EXPECT_EQ(w.bytes().size(), (size_t)5); XI_EXPECT_EQ(w.bytes()[0], tag::Map32); }
    { Writer w; w.array(3); XI_EXPECT_EQ(w.bytes()[0], tag::Array32); }
}

XI_TEST(writer_determinism_and_size_stability) {
    // Same logical frame built twice -> byte-identical.
    auto build = [] {
        Writer w;
        w.map(3);
        w.key("channel"); w.str("line1/cam0");
        w.key("seq");     w.uint_(42);
        w.key("score");   w.float_(0.97);
        return w.take();
    };
    XI_EXPECT(build() == build());

    // Size stability: values on opposite sides of every compact-width boundary
    // encode to the SAME length (the whole point of max-width). 127/128,
    // 255/256, 65535/65536, 2^32-1/2^32 all stay 9 bytes as int64.
    for (uint64_t v : {(uint64_t)127, (uint64_t)128, (uint64_t)255, (uint64_t)256,
                       (uint64_t)65535, (uint64_t)65536, (uint64_t)0xFFFFFFFFull,
                       (uint64_t)0x100000000ull}) {
        Writer w; w.uint_(v);
        XI_EXPECT_EQ(w.bytes().size(), (size_t)9);
        XI_EXPECT_EQ(w.bytes()[0], tag::Int64);
    }
}

XI_TEST(writer_int_and_uint_of_same_value_are_identical) {
    Writer a; a.int_(300);
    Writer b; b.uint_(300);
    XI_EXPECT(a.bytes() == b.bytes());  // canonical value does not depend on signedness
}

// ------------------------------------------------------------------ reader ---

XI_TEST(canonicalize_is_idempotent_on_canonical_input) {
    Writer w;
    w.map(2);
    w.key("a"); w.array(3); w.int_(1); w.int_(-2); w.float_(3.5);
    w.key("b"); w.str("hello");
    Bytes canonical = w.take();

    Writer out;
    Reader r(canonical);
    XI_EXPECT(r.canonicalize(out) == Status::Ok);
    XI_EXPECT(out.bytes() == canonical);  // round-trips to itself
}

XI_TEST(canonicalize_widens_compact_foreign_input) {
    // Hand-built COMPACT msgpack: fixmap(1){ fixstr"n": fixint 5 }.
    Bytes compact = {0x81, 0xA1, 'n', 0x05};

    Writer out;
    Reader r(compact);
    XI_EXPECT(r.canonicalize(out) == Status::Ok);

    // Expected canonical: map32(1){ str32"n": int64 5 }.
    Writer want;
    want.map(1); want.key("n"); want.int_(5);
    XI_EXPECT(out.bytes() == want.bytes());
}

XI_TEST(reader_accepts_all_standard_integer_widths) {
    // uint8 200, uint16 1000, uint32 70000, int8 -5, int64 2^40 — every family
    // the writer never emits but the reader must read (foreign/compact input).
    struct Case { Bytes in; int64_t as_i; bool is_uint; uint64_t as_u; };
    std::vector<Case> cases = {
        {{0xCC, 200},                                  0, true,  200},
        {{0xCD, 0x03, 0xE8},                           0, true,  1000},
        {{0xCE, 0x00, 0x01, 0x11, 0x70},               0, true,  70000},
        {{0xD0, 0xFB},                                -5, false, 0},
        {{0xD3, 0,0,1,0,0,0,0,0},   (int64_t)1<<40, false, 0},
    };
    for (auto& c : cases) {
        Reader r(c.in);
        xi::mp::Element e;
        XI_EXPECT(r.next(e) == Status::Ok);
        if (c.is_uint) { XI_EXPECT(e.kind == xi::mp::Kind::UInt); XI_EXPECT_EQ(e.u, c.as_u); }
        else           { XI_EXPECT(e.kind == xi::mp::Kind::Int);  XI_EXPECT_EQ(e.i, c.as_i); }
    }
}

XI_TEST(reader_str_bin_views_are_zero_copy) {
    Writer w; w.str("payload");
    Bytes buf = w.take();
    Reader r(buf);
    xi::mp::Element e;
    XI_EXPECT(r.next(e) == Status::Ok);
    XI_EXPECT(e.kind == xi::mp::Kind::Str);
    XI_EXPECT_EQ((size_t)e.len, (size_t)7);
    // The view points INTO buf (not a copy).
    XI_EXPECT(e.data >= buf.data() && e.data < buf.data() + buf.size());
    XI_EXPECT_EQ(std::string((const char*)e.data, e.len), std::string("payload"));
}

XI_TEST(ext_policy_reject_accept_strip) {
    // fixext1 type=7 payload=0xAA  ->  d4 07 aa
    Bytes ext = {0xD4, 0x07, 0xAA};

    // default: reject
    XI_EXPECT(xi::mp::validate(ext.data(), ext.size()) == Status::UnexpectedExt);

    // accept-list containing 7: ok, canonicalizes to ext32
    {
        Writer out;
        Reader r(ext);
        XI_EXPECT(r.canonicalize(out, xi::mp::kDefaultMaxDepth,
                                 ExtPolicy::accept_types({7})) == Status::Ok);
        XI_EXPECT_EQ(out.bytes()[0], tag::Ext32);
    }
    // strip_to_bin: the ext becomes a bin32 of the payload
    {
        Writer out;
        Reader r(ext);
        XI_EXPECT(r.canonicalize(out, xi::mp::kDefaultMaxDepth,
                                 ExtPolicy::strip_to_bin()) == Status::Ok);
        XI_EXPECT_EQ(out.bytes()[0], tag::Bin32);
    }
}

// ----------------------------------------------------------------- hostile ---

XI_TEST(reader_rejects_truncated_str_length) {
    // str8 declaring 200 bytes but only 3 follow -> length-overflow.
    Bytes b = {0xD9, 200, 'a', 'b', 'c'};
    XI_EXPECT(xi::mp::validate(b.data(), b.size()) == Status::LengthOverflow);
}

XI_TEST(reader_rejects_truncated_header) {
    // int64 tag with only 2 of 8 payload bytes -> truncated.
    Bytes b = {0xD3, 0x00, 0x00};
    XI_EXPECT(xi::mp::validate(b.data(), b.size()) == Status::Truncated);
}

XI_TEST(reader_rejects_depth_bomb) {
    // 6 nested fixarray(1), then a nil, exceeds a max_depth of 4.
    Bytes b;
    for (int i = 0; i < 6; ++i) b.push_back(0x91);  // fixarray len 1
    b.push_back(0xC0);                               // nil
    XI_EXPECT(xi::mp::validate(b.data(), b.size(), /*max_depth=*/4) == Status::DepthExceeded);
    // The same buffer is fine under a looser bound.
    XI_EXPECT(xi::mp::validate(b.data(), b.size(), /*max_depth=*/16) == Status::Ok);
}

XI_TEST(reader_rejects_trailing_bytes) {
    // A complete nil (0xc0) followed by a stray byte.
    Bytes b = {0xC0, 0xC0};
    XI_EXPECT(xi::mp::validate(b.data(), b.size()) == Status::TrailingBytes);
}

XI_TEST(reader_rejects_reserved_byte) {
    Bytes b = {0xC1};
    XI_EXPECT(xi::mp::validate(b.data(), b.size()) == Status::ReservedByte);
}

XI_TEST(reader_rejects_unexpected_ext) {
    // ext8 len=2 type=9 -> c7 02 09 .. .. (rejected before payload matters)
    Bytes b = {0xC7, 0x02, 0x09, 0x00, 0x00};
    XI_EXPECT(xi::mp::validate(b.data(), b.size()) == Status::UnexpectedExt);
}

XI_TEST(reader_rejects_map_with_missing_value) {
    // map32 declaring 1 pair, but only the key present -> truncated on the value.
    Bytes b = {0xDF, 0,0,0,1, 0xA1, 'k'};  // map32(1) { fixstr"k": <missing> }
    XI_EXPECT(xi::mp::validate(b.data(), b.size()) == Status::Truncated);
}

XI_TEST(reader_huge_count_is_not_a_dos) {
    // array32 declaring 2^32-1 elements but an empty body -> bounded truncation,
    // not a 4-billion-iteration hang.
    Bytes b = {0xDD, 0xFF, 0xFF, 0xFF, 0xFF};
    XI_EXPECT(xi::mp::validate(b.data(), b.size()) == Status::Truncated);
}
