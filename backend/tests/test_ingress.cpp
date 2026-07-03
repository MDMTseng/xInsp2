// test_ingress.cpp — the domain-edge ingress canonicalizer (xi/xi_ingress.hpp).
//
// canonicalize_entry / canonicalize_into are the ONLY public path from foreign,
// untrusted msgpack bytes to a Pack entry (doc 07 "Ingress"). These tests cover
// the three layers and the pool-handle rule:
//   * happy path — compact foreign bytes are validated + widened to canonical
//     and land in a sealed Pack, readable through get_mp.
//   * structural rejection — trailing bytes, non-string keys, duplicate keys,
//     depth bombs are refused (no partial entry reaches the pack).
//   * ext / pool-handle rule — a forged pool-handle ext is refused even when the
//     caller's accept-list names it; a non-handle ext strips to bin under policy.
//   * semantic seam — the optional layer-(c) hook can accept or reject.

#include "xi/xi_ingress.hpp"
#include "xi/xi_pack.hpp"
#include "xi/xi_mp.hpp"

#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

using xi::Pack;
using xi::PackBuilder;
using xi::PackTag;
using xi::mp::Bytes;
using xi::mp::Status;
using xi::mp::Writer;
namespace ingress = xi::ingress;

static std::span<const uint8_t> as_span(const Bytes& b) {
    return std::span<const uint8_t>(b.data(), b.size());
}

// ------------------------------------------------------------------
// Happy path: foreign COMPACT map -> canonical bytes -> a sealed Pack entry.
static void test_ingress_happy_path() {
    // Foreign compact: fixmap(1){ fixstr"n": fixint 5 }.
    Bytes foreign = {0x81, 0xA1, 'n', 0x05};

    ingress::Result r = ingress::canonicalize_entry(as_span(foreign), "mp");
    CHECK(r.ok(), "compact foreign bytes canonicalize ok");

    // Expected canonical: map32(1){ str32"n": int64 5 }.
    Writer want;
    want.map(1); want.key("n"); want.int_(5);
    CHECK(r.canonical == want.bytes(), "canonical output widened to max-width profile");

    // The composed edge lands it in a sealed pack as an opaque Mp entry.
    PackBuilder b;
    ingress::Result r2 = ingress::canonicalize_into(b, "payload", as_span(foreign), "mp");
    CHECK(r2.ok(), "canonicalize_into succeeds");
    Pack f = b.seal();
    CHECK(f.tag_of("payload") == PackTag::Mp, "foreign entry stored as Mp");
    auto got = f.get_mp("payload");
    CHECK(got.has_value(), "Mp entry present");
    CHECK(got && Bytes(got->begin(), got->end()) == want.bytes(),
          "pack stores the canonical bytes verbatim");
}

// ------------------------------------------------------------------
// Structural / profile rejection: nothing malformed reaches a pack.
static void test_ingress_rejects_malformed() {
    // Trailing bytes after a complete value.
    { Bytes b = {0xC0, 0xC0};
      auto r = ingress::canonicalize_entry(as_span(b), "mp");
      CHECK(!r.ok() && r.codec_status == Status::TrailingBytes, "trailing bytes rejected"); }

    // Non-string map key (ruling 2).
    { Bytes b = {0x81, 0x01, 0x02};   // {1: 2}
      auto r = ingress::canonicalize_entry(as_span(b), "mp");
      CHECK(!r.ok() && r.codec_status == Status::NonStringKey, "non-string key rejected"); }

    // Duplicate map key (ruling 5) — rejected at the ingress door.
    { Writer w; w.map(2); w.key("k"); w.int_(1); w.key("k"); w.int_(2);
      Bytes dup = w.take();
      auto r = ingress::canonicalize_entry(as_span(dup), "mp");
      CHECK(!r.ok() && r.codec_status == Status::DuplicateKey, "duplicate key rejected"); }

    // Depth bomb under a tight bound.
    { Bytes b; for (int i = 0; i < 8; ++i) b.push_back(0x91); b.push_back(0xC0);
      ingress::Options opts; opts.max_depth = 4;
      auto r = ingress::canonicalize_entry(as_span(b), "mp", opts);
      CHECK(!r.ok() && r.codec_status == Status::DepthExceeded, "depth bomb rejected"); }
}

// ------------------------------------------------------------------
// Pool-handle ext is NEVER imported — not even via an accept-list.
static void test_ingress_refuses_forged_handle() {
    // A forged pool-handle ext in foreign bytes: fixext1 type=kPoolHandleExtType.
    Bytes forged = {0xD4, (uint8_t)ingress::kPoolHandleExtType, 0xAA};

    // Default reject policy: refused.
    { auto r = ingress::canonicalize_entry(as_span(forged), "image/bgr8");
      CHECK(!r.ok() && r.codec_status == Status::UnexpectedExt,
            "forged pool-handle ext rejected by default"); }

    // Even an accept-list naming the handle code does not import it: ingress
    // strips that code from the effective accept-list (asserts in debug; the
    // defensive strip holds under Release where the assert is compiled out).
    { ingress::Options opts;
      opts.ext_policy = xi::mp::ExtPolicy::strip_to_bin();
      // strip_to_bin turns the forged ext into opaque bin — safe, NOT a handle.
      auto r = ingress::canonicalize_entry(as_span(forged), "image/bgr8", opts);
      CHECK(r.ok(), "strip_to_bin downgrades a forged handle to opaque bin");
      // The result is a bin32 of the payload, never a resolved handle.
      CHECK(!r.canonical.empty() && r.canonical[0] == xi::mp::tag::Bin32,
            "downgraded forged handle is a bin, not an ext/handle"); }
}

// A genuine non-handle ext is still passthrough-able when explicitly accepted,
// proving the handle rule is targeted (not a blanket ext ban at ingress).
static void test_ingress_accepts_nonhandle_ext_when_listed() {
    Bytes ext = {0xD4, 0x07, 0xAA};   // fixext1 type=7
    ingress::Options opts;
    opts.ext_policy = xi::mp::ExtPolicy::accept_types({7});
    auto r = ingress::canonicalize_entry(as_span(ext), "custom", opts);
    CHECK(r.ok(), "an accept-listed non-handle ext passes ingress");
    CHECK(!r.canonical.empty() && r.canonical[0] == xi::mp::tag::Ext32,
          "accepted ext re-emitted as canonical ext32");
}

// ------------------------------------------------------------------
// Layer (c): the optional semantic hook can accept or reject.
static void test_ingress_semantic_hook() {
    Bytes foreign = {0x81, 0xA1, 'n', 0x05};

    // A hook that rejects any "bad/*" tag.
    ingress::Options reject;
    reject.semantic = [](std::string_view tag, const Bytes&) {
        return tag.rfind("bad/", 0) != 0;   // reject tags starting with "bad/"
    };
    { auto r = ingress::canonicalize_entry(as_span(foreign), "bad/thing", reject);
      CHECK(!r.ok() && !r.semantic_ok && r.codec_status == Status::Ok,
            "semantic hook rejects (codec layer still ok)");
      CHECK(r.canonical.empty(), "rejected entry yields no bytes"); }
    { auto r = ingress::canonicalize_entry(as_span(foreign), "good/thing", reject);
      CHECK(r.ok(), "semantic hook accepts a good tag"); }

    // The hook receives the ALREADY-canonical bytes (post widen), not the input.
    ingress::Options inspect;
    bool saw_canonical = false;
    inspect.semantic = [&](std::string_view, const Bytes& canon) {
        saw_canonical = (!canon.empty() && canon[0] == xi::mp::tag::Map32);
        return true;
    };
    ingress::canonicalize_entry(as_span(foreign), "mp", inspect);
    CHECK(saw_canonical, "semantic hook sees canonical (map32) bytes, not the compact input");
}

int main() {
    std::printf("test_ingress\n");
    test_ingress_happy_path();
    test_ingress_rejects_malformed();
    test_ingress_refuses_forged_handle();
    test_ingress_accepts_nonhandle_ext_when_listed();
    test_ingress_semantic_hook();
    if (g_fail == 0) { std::printf("  OK (all checks passed)\n"); return 0; }
    std::printf("  %d check(s) FAILED\n", g_fail);
    return 1;
}
