#pragma once
//
// xi_ingress.hpp — the domain edge for foreign msgpack (polaris2 wave-1, task 1c).
//
// "Trust inside, prove at the boundary." The canonical profile's payoffs (O(1)
// offset reads, one decoder path, sealed immutability) are invariants the
// domain INTERIOR relies on WITHOUT re-checking. This header is the total edge
// that earns that right: it is the ONLY public path from foreign / untrusted
// msgpack bytes (a comms/MES plugin's inbound payloads, third-party camera
// chunk data, json_source-style feeds, old replay files) to a Frame entry. See
// docs/new_gen/07-uniform-keyed-buffer-plane.md ("Ingress").
//
// It sits ABOVE both siblings — xi_mp.hpp (the codec) and xi_frame.hpp (the
// container) — and composes them into the three-layer ingress of doc 07:
//
//   (a) STRUCTURAL well-formedness — bounded nesting depth, declared-vs-actual
//       lengths checked before allocation, no trailing bytes, string-keyed
//       maps. (mp::Reader's validating walk.)
//   (b) PROFILE normalization — compact widths widened to canonical max-width,
//       NaN flattened to the one canonical pattern, duplicate map keys rejected.
//       (mp::canonicalize; it subsumes layer (a)'s checks, so both are ONE pass
//       — exactly doc 07's "decoded, validated, and re-encoded in one pass".)
//   (c) SEMANTIC validation — an OPTIONAL contract-schema check against the
//       entry's claimed type tag. Full schema validation at ingress is a later
//       task; this is the callback seam it plugs into (default: accept).
//
// EXT / POOL-HANDLE RULE. A pool handle is an msgpack ext value; a forged
// handle in foreign data would be a fabricated pointer into the pool
// (use-after-free / type confusion by construction). Handles are mintable ONLY
// by the frame layer's own allocator (xi_frame's frame_pool). Therefore ingress
// NEVER imports a pool-handle ext — not even when a caller's accept-list names
// it. That is asserted (and, belt-and-braces, stripped from the effective
// accept-list so the guarantee holds under NDEBUG too). Non-handle foreign ext
// is rejected by default or downgraded to bin per policy.

#include "xi_frame.hpp"
#include "xi_mp.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace xi {
namespace ingress {

// The reserved msgpack ext type code for a pool handle ("pool ref"). Minted
// ONLY by the frame layer's allocator; NEVER imported from foreign bytes. The
// value is a domain reservation (application-specific ext range, 0..127).
constexpr int8_t kPoolHandleExtType = 0x70;

// Layer-(c) seam. When an entry claims a known type tag, a later task will
// validate its (already-canonical) bytes against the contract schema. This is
// the hook that work plugs into; return true to accept, false to reject. Full
// schema validation at ingress is intentionally OUT OF SCOPE for task 1c.
using SemanticValidator =
    std::function<bool(std::string_view type_tag, const mp::Bytes& canonical)>;

struct Options {
    int           max_depth  = mp::kDefaultMaxDepth;
    mp::ExtPolicy ext_policy  = mp::ExtPolicy::reject_all();  // reject or strip_to_bin
    SemanticValidator semantic = nullptr;                     // optional layer (c)
};

// The outcome of ingress. `canonical` is meaningful ONLY when ok(): the caller
// must discard it on any failure. This is a DISTINCT type from a raw byte span:
// it is the only token that FrameBuilder's foreign-entry path accepts, so
// untrusted bytes cannot reach a frame except through canonicalize_entry.
struct Result {
    mp::Status codec_status = mp::Status::Ok;  // layers (a)+(b): the codec verdict
    bool       semantic_ok  = true;            // layer (c): the schema-hook verdict
    mp::Bytes  canonical;                      // valid IFF ok()

    bool ok() const { return codec_status == mp::Status::Ok && semantic_ok; }
    explicit operator bool() const { return ok(); }
};

// Canonicalize foreign msgpack bytes into a Frame-insertable Result. `type_tag`
// is the entry's claimed type (fed to the optional semantic hook; storage is
// opaque canonical msgpack either way). On any failure the Result is not ok()
// and its bytes are empty.
inline Result canonicalize_entry(std::span<const uint8_t> foreign,
                                 std::string_view type_tag,
                                 const Options& opts = {}) {
    // A pool-handle ext is minted, never imported. A caller must not accept-list
    // it; assert they did not, then strip it defensively so the guarantee holds
    // even where the assert is compiled out (NDEBUG / Release).
    for (int8_t a : opts.ext_policy.accept)
        assert(a != kPoolHandleExtType &&
               "pool-handle ext is minted by the frame layer, never imported at ingress");
    mp::ExtPolicy policy = opts.ext_policy;
    policy.accept.erase(std::remove(policy.accept.begin(), policy.accept.end(),
                                    kPoolHandleExtType),
                        policy.accept.end());

    Result r;

    // Layers (a)+(b) in one validated pass: canonicalize enforces the same
    // structural bounds validate() does AND applies the profile (widen, NaN
    // flatten, dup-key + non-string-key rejection, ext policy).
    mp::Writer out;
    r.codec_status = mp::canonicalize(foreign.data(), foreign.size(), out,
                                      opts.max_depth, policy);
    if (r.codec_status != mp::Status::Ok) return r;
    r.canonical = out.take();

    // Layer (c): optional semantic validation against the claimed type tag.
    if (opts.semantic && !opts.semantic(type_tag, r.canonical)) {
        r.semantic_ok = false;
        r.canonical.clear();
    }
    return r;
}

// Convenience: canonicalize foreign bytes and, on success, add them to `b` as
// an opaque canonical-msgpack (Mp) entry under `key`. Returns the ingress
// Result so the caller can inspect the failure reason. This is the composed
// "foreign bytes -> sealed frame entry" edge in one call.
inline Result canonicalize_into(FrameBuilder& b, std::string_view key,
                                std::span<const uint8_t> foreign,
                                std::string_view type_tag,
                                const Options& opts = {}) {
    Result r = canonicalize_entry(foreign, type_tag, opts);
    if (r.ok()) b.add_mp(key, r.canonical.data(), r.canonical.size());
    return r;
}

}  // namespace ingress
}  // namespace xi
