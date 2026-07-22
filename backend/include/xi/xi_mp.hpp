// xi_mp.hpp — the canonical msgpack codec for the xInsp3 (v3) pack plane.
//
// This is the foundation of the "uniform keyed binary buffer" data plane
// (docs/new_gen/07-uniform-keyed-buffer-plane.md). It is HEADER-ONLY and has
// ZERO dependencies (no yyjson, no OpenCV) so it can be included from the core,
// from plugins, and from the future ingress edge alike.
//
// It implements three things and nothing else:
//
//   1. Writer — a builder that emits ONLY the canonical max-width profile. Every
//      number is fixed width (int64 0xd3, or uint64 0xcf strictly beyond int64
//      range; float64 0xcb), every container/str/bin marker is the widest form
//      (map32/array32/str32/bin32). The output is 100% standard msgpack — any
//      stock decoder reads it — the writer simply never compacts. The default
//      builder CANNOT emit ext; ext is a separate, clearly-privileged method
//      (Writer::ext_privileged) reserved for the pack layer's pool-handle mint.
//
//   2. Reader — a BOUNDED, VALIDATING pull decoder over the FULL standard
//      msgpack width families (so it reads both our canonical output AND foreign
//      compact input — reading compact is what the canonicalizer needs). It
//      checks declared-length vs remaining-bytes BEFORE it advances, bounds
//      nesting depth, and rejects trailing bytes at the top level. str/bin/ext
//      payloads are returned as ZERO-COPY spans into the caller's input buffer.
//      Ext policy is parameterized: REJECT by default, accept-listed types for
//      privileged callers, or strip-to-bin.
//
//   3. Canonicalizer — read-any → write-canonical in one pass (the future
//      ingress core). Given a foreign buffer it produces the canonical encoding,
//      rejecting or stripping ext per policy.
//
// CANONICAL FIELD ORDER IS THE CALLER'S CONTRACT DUTY. The Writer emits map
// entries in the exact order the caller appends key/value pairs; it does not
// sort. Two producers that agree on a schema must agree on field order to
// produce byte-identical frames — that agreement lives in the contract layer,
// not here. The canonicalizer likewise PRESERVES a foreign map's key order
// (schema-aware field-order normalization is a higher layer's job).
//
// The exact profile decisions (and every place doc 07 was ambiguous) are
// recorded in contract/canonical-profile-notes.md — the tie-breaker record for
// the cross-implementation (TS/Python) byte-compare.
#ifndef XI_MP_HPP
#define XI_MP_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace xi {
namespace mp {

using Bytes = std::vector<uint8_t>;

// The ONLY tags the canonical writer emits. (The reader accepts far more.)
namespace tag {
constexpr uint8_t Nil     = 0xc0;
constexpr uint8_t False   = 0xc2;
constexpr uint8_t True    = 0xc3;
constexpr uint8_t Bin32   = 0xc6;
constexpr uint8_t Ext32   = 0xc9;
constexpr uint8_t Float64 = 0xcb;
constexpr uint8_t UInt64  = 0xcf;
constexpr uint8_t Int64   = 0xd3;
constexpr uint8_t Str32   = 0xdb;
constexpr uint8_t Array32 = 0xdd;
constexpr uint8_t Map32   = 0xdf;
}  // namespace tag

// Big-endian appenders (msgpack multi-byte integers are big-endian).
inline void put_be32(Bytes& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back((uint8_t)(v >> s));
}
inline void put_be64(Bytes& b, uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) b.push_back((uint8_t)(v >> s));
}

// ---------------------------------------------------------------------------
// Writer — emits ONLY the canonical max-width profile.
// ---------------------------------------------------------------------------
//
// The scalar/str/bin/map/array methods below are the WHOLE default builder
// surface, and NONE of them can emit an ext value. Building a map is: call
// map(n), then append exactly n key+value pairs, in the caller's chosen
// (contract) order. The Writer trusts the caller to match the declared count;
// it does not track it (the reader is where malformed nesting is caught).
class Writer {
public:
    void nil()             { buf_.push_back(tag::Nil); }
    void boolean(bool v)   { buf_.push_back(v ? tag::True : tag::False); }

    // Signed integers always encode as int64 (0xd3).
    void int_(int64_t v)   { buf_.push_back(tag::Int64); put_be64(buf_, (uint64_t)v); }

    // A non-negative value in [0, 2^63) encodes as int64 (0xd3) — the canonical
    // form for that value regardless of the caller's C++ signedness. Only a
    // value strictly beyond INT64_MAX takes uint64 (0xcf).
    void uint_(uint64_t v) {
        if (v <= (uint64_t)INT64_MAX) { buf_.push_back(tag::Int64);  put_be64(buf_, v); }
        else                          { buf_.push_back(tag::UInt64); put_be64(buf_, v); }
    }

    // All floats encode as float64 (0xcb). NaN is NORMALIZED to the single
    // canonical quiet-NaN pattern 0x7ff8000000000000 (binding ruling 1,
    // 2026-07-02): IEEE-754 has ~2^53 NaN encodings, so a byte-deterministic
    // profile must pick one, and cross-language determinism (TS/Py) wins over
    // bit-pattern preservation. ANY NaN — any sign, any payload, signalling or
    // quiet — flattens; +/-Inf and -0.0 are preserved exactly. This same writer
    // is the canonicalizer's emit path, so foreign NaN is flattened on ingress
    // too. NaN is detected on the RAW bits (exponent all-ones, mantissa
    // non-zero) rather than std::isnan, so a signalling NaN is caught without
    // relying on the FPU not to quiet it in transit.
    void float_(double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        if ((bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull &&
            (bits & 0x000fffffffffffffull) != 0)
            bits = 0x7ff8000000000000ull;
        buf_.push_back(tag::Float64);
        put_be64(buf_, bits);
    }

    // Strings and byte blobs always take the widest marker (str32 / bin32).
    void str(std::string_view s) {
        buf_.push_back(tag::Str32);
        put_be32(buf_, (uint32_t)s.size());
        buf_.insert(buf_.end(), s.begin(), s.end());
    }
    void bin(const uint8_t* p, size_t n) {
        buf_.push_back(tag::Bin32);
        put_be32(buf_, (uint32_t)n);
        buf_.insert(buf_.end(), p, p + n);
    }

    // Containers always take the widest marker (map32 / array32).
    void map(uint32_t n)   { buf_.push_back(tag::Map32);   put_be32(buf_, n); }
    void array(uint32_t n) { buf_.push_back(tag::Array32); put_be32(buf_, n); }

    // Convenience: a map key is just a canonical string.
    void key(std::string_view k) { str(k); }

    // --- count-free containers (backpatch) --------------------------------
    // Canonical msgpack needs the element count in the map32/array32 header,
    // but a sequential builder only knows it at close. Because the canonical
    // profile ALWAYS uses the fixed-width map32/array32 header (5 bytes), the
    // count field is at a fixed offset and can be backpatched in place:
    //   size_t at = w.open_map();   // header with a 0 placeholder; remember `at`
    //   ... append `n` key+value pairs ...
    //   w.set_count(at, n);         // fix the count
    // The final bytes are byte-identical to map(n)/array(n), so canonical
    // cross-language parity is preserved. `at` is the offset of the 4-byte
    // big-endian count (just past the tag byte).
    size_t open_map()   { buf_.push_back(tag::Map32);   size_t at = buf_.size(); put_be32(buf_, 0); return at; }
    size_t open_array() { buf_.push_back(tag::Array32); size_t at = buf_.size(); put_be32(buf_, 0); return at; }
    void set_count(size_t at, uint32_t n) {
        buf_[at]     = (uint8_t)(n >> 24);
        buf_[at + 1] = (uint8_t)(n >> 16);
        buf_[at + 2] = (uint8_t)(n >> 8);
        buf_[at + 3] = (uint8_t)(n);
    }
#ifndef NDEBUG
    // Interleave guard for the count-free builders (xi_mp_build.hpp): the
    // innermost still-open builder. Debug-only, zero release footprint. A
    // builder asserts it equals `this` before every write, so writing to a
    // parent while a child container is still open trips at the call site.
    const void* dbg_leaf_ = nullptr;
#endif

    // PRIVILEGED. The default builder above cannot emit ext; this is the SOLE
    // ext path. It exists for (a) the pack layer minting a pool-handle ext and
    // (b) the canonicalizer passing an accept-listed ext through. Untrusted
    // producers never reach it. Canonical ext width is ext32 (0xc9) always.
    void ext_privileged(int8_t ext_type, const uint8_t* data, size_t len) {
        buf_.push_back(tag::Ext32);
        put_be32(buf_, (uint32_t)len);
        buf_.push_back((uint8_t)ext_type);
        buf_.insert(buf_.end(), data, data + len);
    }

    // Splice an already-canonical, self-contained msgpack value (ONE complete
    // scalar / str / bin / map / array subtree) into the stream VERBATIM — no
    // re-encoding. This is the frame-dump primitive: an entry's stored canonical
    // bytes (the in-memory small plane) land on the wire byte-for-byte, so
    // memory ≈ wire is an identity, not a conversion (doc 07 "boundaries become
    // copies, not transformations"). The caller guarantees the bytes are exactly
    // one canonical value already in the max-width profile (e.g. from a frame
    // arena or another Writer); this method trusts them, it does not validate.
    void raw_canonical(const uint8_t* p, size_t n) {
        if (n) buf_.insert(buf_.end(), p, p + n);
    }

    const Bytes& bytes() const { return buf_; }
    Bytes take()               { return std::move(buf_); }
    void clear()               { buf_.clear(); }
    size_t size() const        { return buf_.size(); }
    bool empty() const         { return buf_.empty(); }

private:
    Bytes buf_;
};

// ---------------------------------------------------------------------------
// Reader — bounded, validating, zero-copy pull decoder.
// ---------------------------------------------------------------------------

enum class Status {
    Ok = 0,
    Truncated,       // a header or fixed payload runs past the end of the buffer
    ReservedByte,    // the never-used byte 0xc1
    DepthExceeded,   // nesting deeper than max_depth
    LengthOverflow,  // a declared str/bin/ext length exceeds the remaining bytes
    TrailingBytes,   // extra bytes after the top-level value
    UnexpectedExt,   // an ext type not on the accept-list (policy = reject)
    NonStringKey,    // a map key that is not a str (ruling 2 — string-keyed maps)
    DuplicateKey,    // a map with a repeated key (ruling 5 — canonicalize only)
};

inline const char* status_str(Status s) {
    switch (s) {
        case Status::Ok:             return "ok";
        case Status::Truncated:      return "truncated";
        case Status::ReservedByte:   return "reserved-byte";
        case Status::DepthExceeded:  return "depth-exceeded";
        case Status::LengthOverflow: return "length-overflow";
        case Status::TrailingBytes:  return "trailing-bytes";
        case Status::UnexpectedExt:  return "unexpected-ext";
        case Status::NonStringKey:   return "non-string-key";
        case Status::DuplicateKey:   return "duplicate-key";
    }
    return "?";
}

enum class Kind { Nil, Bool, Int, UInt, Float, Str, Bin, Array, Map, Ext };

// One decoded element. For Str/Bin/Ext, {data,len} is a zero-copy VIEW into the
// Reader's input buffer (valid only while that buffer lives). For Array/Map,
// `len` is the element/pair count and the cursor is left at the first child.
struct Element {
    Kind           kind     = Kind::Nil;
    bool           b        = false;
    int64_t        i        = 0;
    uint64_t       u        = 0;
    double         d        = 0;
    const uint8_t* data     = nullptr;  // Str/Bin/Ext payload view
    uint32_t       len      = 0;        // Str/Bin/Ext byte length, or Array/Map count
    int8_t         ext_type = 0;
};

// Ext handling policy. By default NOTHING is accepted and foreign ext is
// REJECTED — the safe default the ingress edge relies on (a forged pool handle
// in untrusted data would be a fabricated pointer). Privileged callers pass an
// accept-list; the canonicalizer can alternatively strip unknown ext to bin.
struct ExtPolicy {
    enum class OnForeign { Reject, StripToBin };

    OnForeign            on_foreign = OnForeign::Reject;
    std::vector<int8_t>  accept;  // ext types allowed to pass through unchanged

    bool accepts(int8_t t) const {
        for (int8_t a : accept) if (a == t) return true;
        return false;
    }

    static ExtPolicy reject_all()                    { return ExtPolicy{}; }
    static ExtPolicy accept_types(std::vector<int8_t> ts) {
        ExtPolicy p; p.accept = std::move(ts); return p;
    }
    static ExtPolicy strip_to_bin() {
        ExtPolicy p; p.on_foreign = OnForeign::StripToBin; return p;
    }
};

// Default nesting bound. Frames are shallow (a map of typed entries, each a
// small map/array); 64 is generous headroom while still killing a depth bomb.
constexpr int kDefaultMaxDepth = 64;

class Reader {
public:
    Reader(const uint8_t* data, size_t size)
        : beg_(data), p_(data), end_(data + size) {}
    explicit Reader(const Bytes& b) : Reader(b.data(), b.size()) {}
    Reader(std::string_view s)
        : Reader(reinterpret_cast<const uint8_t*>(s.data()), s.size()) {}

    size_t offset() const { return (size_t)(p_ - beg_); }
    bool at_end() const   { return p_ >= end_; }
    void rewind()         { p_ = beg_; }

    // Decode ONE element header at the cursor. Advances past the header and, for
    // Str/Bin/Ext, past the payload (setting a zero-copy view). For Array/Map,
    // sets len = count and leaves the cursor at the first child. This is purely
    // STRUCTURAL: it applies no ext policy and no depth bound (the recursive
    // validate()/canonicalize() walkers below apply those).
    Status next(Element& out) {
        if (p_ >= end_) return Status::Truncated;
        out = Element{};
        uint8_t c = *p_++;

        if (c <= 0x7f) { out.kind = Kind::Int; out.i = (int64_t)c; return Status::Ok; }   // positive fixint
        if (c >= 0xe0) { out.kind = Kind::Int; out.i = (int64_t)(int8_t)c; return Status::Ok; }  // negative fixint
        if (c >= 0x80 && c <= 0x8f) { out.kind = Kind::Map;   out.len = (uint32_t)(c & 0x0f); return Status::Ok; }  // fixmap
        if (c >= 0x90 && c <= 0x9f) { out.kind = Kind::Array; out.len = (uint32_t)(c & 0x0f); return Status::Ok; }  // fixarray
        if (c >= 0xa0 && c <= 0xbf) { return read_str(out, (uint64_t)(c & 0x1f)); }        // fixstr

        switch (c) {
            case 0xc0: out.kind = Kind::Nil;  return Status::Ok;                            // nil
            case 0xc1: return Status::ReservedByte;                                         // never used
            case 0xc2: out.kind = Kind::Bool; out.b = false; return Status::Ok;             // false
            case 0xc3: out.kind = Kind::Bool; out.b = true;  return Status::Ok;             // true
            case 0xc4: return read_len_then_bin(out, 1);                                    // bin8
            case 0xc5: return read_len_then_bin(out, 2);                                    // bin16
            case 0xc6: return read_len_then_bin(out, 4);                                    // bin32
            case 0xc7: return read_ext(out, 1);                                             // ext8
            case 0xc8: return read_ext(out, 2);                                             // ext16
            case 0xc9: return read_ext(out, 4);                                             // ext32
            case 0xca: return read_float32(out);                                            // float32
            case 0xcb: return read_float64(out);                                            // float64
            case 0xcc: return read_uint(out, 1);                                            // uint8
            case 0xcd: return read_uint(out, 2);                                            // uint16
            case 0xce: return read_uint(out, 4);                                            // uint32
            case 0xcf: return read_uint(out, 8);                                            // uint64
            case 0xd0: return read_int(out, 1);                                             // int8
            case 0xd1: return read_int(out, 2);                                             // int16
            case 0xd2: return read_int(out, 4);                                             // int32
            case 0xd3: return read_int(out, 8);                                             // int64
            case 0xd4: return read_fixext(out, 1);                                          // fixext1
            case 0xd5: return read_fixext(out, 2);                                          // fixext2
            case 0xd6: return read_fixext(out, 4);                                          // fixext4
            case 0xd7: return read_fixext(out, 8);                                          // fixext8
            case 0xd8: return read_fixext(out, 16);                                         // fixext16
            case 0xd9: return read_len_then_str(out, 1);                                    // str8
            case 0xda: return read_len_then_str(out, 2);                                    // str16
            case 0xdb: return read_len_then_str(out, 4);                                    // str32
            case 0xdc: return read_count(out, Kind::Array, 2);                              // array16
            case 0xdd: return read_count(out, Kind::Array, 4);                              // array32
            case 0xde: return read_count(out, Kind::Map, 2);                                // map16
            case 0xdf: return read_count(out, Kind::Map, 4);                                // map32
        }
        return Status::ReservedByte;  // unreachable (all 256 bytes handled above)
    }

    // Walk the WHOLE top-level value, enforcing depth and ext policy, and require
    // that nothing follows it. Structural well-formedness of the entire buffer.
    // Map keys must be strings (ruling 2 -> NonStringKey); duplicate keys are
    // NOT checked here (that hygiene check lives at the ingress door,
    // canonicalize(); see below).
    Status validate(int max_depth = kDefaultMaxDepth,
                    const ExtPolicy& policy = ExtPolicy::reject_all()) {
        rewind();
        Status s = walk_validate(0, max_depth, policy);
        if (s != Status::Ok) return s;
        if (p_ != end_) return Status::TrailingBytes;
        return Status::Ok;
    }

    // Read-any → write-canonical, in one pass. On success `out` holds the
    // canonical re-encoding of the whole buffer. Enforces the same bounds as
    // validate() PLUS the two ingress-door checks: NaN is flattened to the
    // canonical pattern (ruling 1, via the Writer), non-string map keys are
    // rejected (ruling 2 -> NonStringKey), and DUPLICATE map keys are rejected
    // (ruling 5 -> DuplicateKey; this is the strict half of the split with
    // validate()). An accept-listed ext passes through (re-emitted as ext32), a
    // foreign ext is rejected or stripped to bin per policy.
    Status canonicalize(Writer& out, int max_depth = kDefaultMaxDepth,
                        const ExtPolicy& policy = ExtPolicy::reject_all()) {
        rewind();
        Status s = walk_canon(out, 0, max_depth, policy);
        if (s != Status::Ok) return s;
        if (p_ != end_) return Status::TrailingBytes;
        return Status::Ok;
    }

private:
    const uint8_t* beg_;
    const uint8_t* p_;
    const uint8_t* end_;

    size_t remaining() const { return (size_t)(end_ - p_); }

    // Read `n` big-endian bytes at the cursor into `v`. False if truncated.
    bool take_be(size_t n, uint64_t& v) {
        if (remaining() < n) return false;
        v = 0;
        for (size_t k = 0; k < n; ++k) v = (v << 8) | *p_++;
        return true;
    }

    Status read_uint(Element& out, size_t n) {
        uint64_t v;
        if (!take_be(n, v)) return Status::Truncated;
        out.kind = Kind::UInt; out.u = v;
        return Status::Ok;
    }
    Status read_int(Element& out, size_t n) {
        uint64_t raw;
        if (!take_be(n, raw)) return Status::Truncated;
        int64_t v;
        switch (n) {
            case 1:  v = (int8_t)raw;  break;
            case 2:  v = (int16_t)raw; break;
            case 4:  v = (int32_t)raw; break;
            default: v = (int64_t)raw; break;
        }
        out.kind = Kind::Int; out.i = v;
        return Status::Ok;
    }
    Status read_float32(Element& out) {
        uint64_t raw;
        if (!take_be(4, raw)) return Status::Truncated;
        uint32_t bits = (uint32_t)raw;
        float f;
        std::memcpy(&f, &bits, sizeof f);
        out.kind = Kind::Float; out.d = (double)f;
        return Status::Ok;
    }
    Status read_float64(Element& out) {
        uint64_t raw;
        if (!take_be(8, raw)) return Status::Truncated;
        double dd;
        std::memcpy(&dd, &raw, sizeof dd);
        out.kind = Kind::Float; out.d = dd;
        return Status::Ok;
    }

    // A str/bin payload: check declared length against remaining bytes BEFORE
    // taking the view (the "prove it fits before you trust it" edge check).
    Status read_str(Element& out, uint64_t len) {
        if (remaining() < len) return Status::LengthOverflow;
        out.kind = Kind::Str; out.data = p_; out.len = (uint32_t)len;
        p_ += len;
        return Status::Ok;
    }
    Status read_bin(Element& out, uint64_t len) {
        if (remaining() < len) return Status::LengthOverflow;
        out.kind = Kind::Bin; out.data = p_; out.len = (uint32_t)len;
        p_ += len;
        return Status::Ok;
    }
    Status read_len_then_str(Element& out, size_t lensize) {
        uint64_t len;
        if (!take_be(lensize, len)) return Status::Truncated;
        return read_str(out, len);
    }
    Status read_len_then_bin(Element& out, size_t lensize) {
        uint64_t len;
        if (!take_be(lensize, len)) return Status::Truncated;
        return read_bin(out, len);
    }

    // ext8/16/32: length (lensize bytes), then a 1-byte type, then the payload.
    Status read_ext(Element& out, size_t lensize) {
        uint64_t len;
        if (!take_be(lensize, len)) return Status::Truncated;
        if (p_ >= end_) return Status::Truncated;               // type byte
        int8_t t = (int8_t)*p_++;
        if (remaining() < len) return Status::LengthOverflow;
        out.kind = Kind::Ext; out.ext_type = t; out.data = p_; out.len = (uint32_t)len;
        p_ += len;
        return Status::Ok;
    }
    // fixext1..16: a 1-byte type, then exactly `n` payload bytes.
    Status read_fixext(Element& out, uint32_t n) {
        if (p_ >= end_) return Status::Truncated;               // type byte
        int8_t t = (int8_t)*p_++;
        if (remaining() < n) return Status::LengthOverflow;
        out.kind = Kind::Ext; out.ext_type = t; out.data = p_; out.len = n;
        p_ += n;
        return Status::Ok;
    }
    Status read_count(Element& out, Kind k, size_t lensize) {
        uint64_t n;
        if (!take_be(lensize, n)) return Status::Truncated;
        out.kind = k; out.len = (uint32_t)n;
        return Status::Ok;
    }

    // NB on the container loops below: a bogus huge count (e.g. array32 declaring
    // 2^32-1 elements) is NOT a DoS — each child consumes >= 1 byte, so at most
    // `remaining()` iterations succeed before next() returns Truncated and we
    // bail. The work is bounded by the buffer size, not the declared count.
    Status walk_validate(int depth, int max_depth, const ExtPolicy& policy) {
        Element e;
        Status s = next(e);
        if (s != Status::Ok) return s;
        switch (e.kind) {
            case Kind::Ext:
                if (!policy.accepts(e.ext_type) &&
                    policy.on_foreign == ExtPolicy::OnForeign::Reject)
                    return Status::UnexpectedExt;
                return Status::Ok;
            case Kind::Array: {
                if (depth + 1 > max_depth) return Status::DepthExceeded;
                for (uint32_t k = 0; k < e.len; ++k) {
                    s = walk_validate(depth + 1, max_depth, policy);
                    if (s != Status::Ok) return s;
                }
                return Status::Ok;
            }
            case Kind::Map: {
                if (depth + 1 > max_depth) return Status::DepthExceeded;
                for (uint32_t k = 0; k < e.len; ++k) {
                    // Ruling 2: the pack plane is string-keyed. The key must be
                    // a str scalar (a leaf) — read it directly and reject any
                    // foreign non-string key. validate() stays PERMISSIVE on
                    // duplicate keys (ruling 5 puts dup-rejection at the ingress
                    // door, canonicalize(); cheap dup-detection here is awkward
                    // and structural validity does not require it).
                    Element key;
                    s = next(key);
                    if (s != Status::Ok) return s;
                    if (key.kind != Kind::Str) return Status::NonStringKey;
                    s = walk_validate(depth + 1, max_depth, policy);  // value
                    if (s != Status::Ok) return s;
                }
                return Status::Ok;
            }
            default:
                return Status::Ok;
        }
    }

    Status walk_canon(Writer& w, int depth, int max_depth, const ExtPolicy& policy) {
        Element e;
        Status s = next(e);
        if (s != Status::Ok) return s;
        switch (e.kind) {
            case Kind::Nil:   w.nil();          return Status::Ok;
            case Kind::Bool:  w.boolean(e.b);   return Status::Ok;
            case Kind::Int:   w.int_(e.i);      return Status::Ok;
            case Kind::UInt:  w.uint_(e.u);     return Status::Ok;
            case Kind::Float: w.float_(e.d);    return Status::Ok;
            case Kind::Str:   w.str(std::string_view((const char*)e.data, e.len)); return Status::Ok;
            case Kind::Bin:   w.bin(e.data, e.len); return Status::Ok;
            case Kind::Ext:
                if (policy.accepts(e.ext_type)) {
                    w.ext_privileged(e.ext_type, e.data, e.len);
                    return Status::Ok;
                }
                if (policy.on_foreign == ExtPolicy::OnForeign::StripToBin) {
                    w.bin(e.data, e.len);
                    return Status::Ok;
                }
                return Status::UnexpectedExt;
            case Kind::Array: {
                if (depth + 1 > max_depth) return Status::DepthExceeded;
                w.array(e.len);
                for (uint32_t k = 0; k < e.len; ++k) {
                    s = walk_canon(w, depth + 1, max_depth, policy);
                    if (s != Status::Ok) return s;
                }
                return Status::Ok;
            }
            case Kind::Map: {
                if (depth + 1 > max_depth) return Status::DepthExceeded;
                w.map(e.len);
                // canonicalize() is the ingress door, so it is STRICT where
                // validate() is permissive: it both enforces string keys
                // (ruling 2) and rejects duplicate keys (ruling 5). The seen-set
                // holds zero-copy views into the input buffer, valid for the
                // duration of this walk.
                std::unordered_set<std::string_view> seen;
                // Clamp to what the buffer can actually hold: e.len is the
                // DECLARED map count (up to 2^32-1 for map32) and a bogus huge
                // count would otherwise reserve tens of GB before a single
                // entry is read -> bad_alloc -> abort. Each entry costs >= 1
                // byte, so remaining() is the true upper bound (matches the
                // container-loop DoS note above).
                seen.reserve(std::min<size_t>(e.len, remaining()));
                for (uint32_t k = 0; k < e.len; ++k) {
                    Element key;
                    s = next(key);
                    if (s != Status::Ok) return s;
                    if (key.kind != Kind::Str) return Status::NonStringKey;
                    std::string_view ksv((const char*)key.data, key.len);
                    if (!seen.insert(ksv).second) return Status::DuplicateKey;
                    w.str(ksv);
                    s = walk_canon(w, depth + 1, max_depth, policy);  // value
                    if (s != Status::Ok) return s;
                }
                return Status::Ok;
            }
        }
        return Status::Ok;  // unreachable
    }
};

// ---- free-function conveniences --------------------------------------------

// Validate a whole buffer. Ok iff structurally well-formed within the bounds.
inline Status validate(const uint8_t* data, size_t size,
                       int max_depth = kDefaultMaxDepth,
                       const ExtPolicy& policy = ExtPolicy::reject_all()) {
    Reader r(data, size);
    return r.validate(max_depth, policy);
}

// Canonicalize a whole foreign buffer into `out`. Ok iff it validated and
// re-encoded; on any error `out` holds partial bytes and MUST be discarded.
inline Status canonicalize(const uint8_t* data, size_t size, Writer& out,
                           int max_depth = kDefaultMaxDepth,
                           const ExtPolicy& policy = ExtPolicy::reject_all()) {
    Reader r(data, size);
    return r.canonicalize(out, max_depth, policy);
}

}  // namespace mp
}  // namespace xi

#endif  // XI_MP_HPP
