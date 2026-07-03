// xex1_pack_load.hpp — read a canonical XEX1-v2 dump back into a sealed xi::Pack.
//
// This is the REPLAY half of doc 10 gate P3 (persistence parity): record_save's
// pack door writes the canonical dump (memory ≈ wire ≈ disk); this reads it back.
// It is a HOST-side utility (it mints pool handles + builds an xi::Pack), so it
// lives beside the format code but is consumed by the replay test / a future
// replay source, NOT by the record_save plugin itself (a plugin cannot build a
// host pack directly — full replay-source integration is the follow-up).
//
// DISK IS UNTRUSTED (doc 07 "Ingress"). A dump file may be truncated, hand-forged,
// or an old/foreign artifact. So the body does NOT go straight to add_mp (which
// TRUSTS its bytes) — it goes through xi::ingress::canonicalize_entry, the ONE
// blessed edge: bounded nesting, declared-vs-actual length checks, string-keyed +
// dup-key rejection, and — critically — a forged pool-handle ext is refused
// (a fabricated pointer into the pool). Only after the WHOLE body clears ingress
// do we splice its now-trusted canonical sub-values into the rebuilt pack.
//
// FORMAT NOTE. XEX1-v2 carries an image as a msgpack map {w,h,c,px:<bin>}, so on
// readback an image and a genuine nested-msgpack map are distinguished by SHAPE:
// a 4-key map whose keys are exactly {w,h,c,px} (ints + a bin) is rebuilt as an
// IMAGE entry, anything else as an opaque Mp entry. The original entry TAG is not
// on the wire (doc 07 D2: images are descriptors over raw buffers), so this shape
// test is how the tag is recovered; a real nested map shaped exactly like an image
// descriptor is the one ambiguous case and is documented as such.
#ifndef XI_XEX1_PACK_LOAD_HPP
#define XI_XEX1_PACK_LOAD_HPP

#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <xi/xi_ingress.hpp>  // xi::ingress::canonicalize_entry (the untrusted edge)
#include <xi/xi_mp.hpp>       // xi::mp::Reader (decode the trusted canonical body)
#include <xi/xi_pack.hpp>     // xi::Pack / xi::PackBuilder (rebuild the sealed pack)

namespace xi {
namespace xex1 {

// The outcome of a load. `pack` is meaningful only when ok(); channel/seq carry
// the reserved fields lifted back out of the frame header.
struct LoadResult {
    bool        ok = false;
    std::string error;      // human reason when !ok
    std::string channel;
    uint64_t    seq = 0;
    xi::Pack    pack;

    explicit operator bool() const { return ok; }
};

namespace load_detail {

// Consume ONE complete msgpack value (header + full body) at the reader cursor.
// Used to measure a frame value's canonical byte span [start,end).
inline mp::Status skip_value(mp::Reader& r) {
    mp::Element e;
    mp::Status s = r.next(e);
    if (s != mp::Status::Ok) return s;
    if (e.kind == mp::Kind::Array) {
        for (uint32_t i = 0; i < e.len; ++i) { s = skip_value(r); if (s != mp::Status::Ok) return s; }
    } else if (e.kind == mp::Kind::Map) {
        for (uint32_t i = 0; i < e.len; ++i) {
            s = skip_value(r); if (s != mp::Status::Ok) return s;  // key
            s = skip_value(r); if (s != mp::Status::Ok) return s;  // value
        }
    }
    return mp::Status::Ok;
}

// Is the canonical value at [p,p+n) an image descriptor map {w,h,c,px}? On yes,
// fill dims + the (zero-copy) pixel span and return true.
inline bool as_image(const uint8_t* p, size_t n,
                     int32_t& w, int32_t& h, int32_t& c,
                     const uint8_t*& px, uint32_t& px_len) {
    mp::Reader r(p, n);
    mp::Element top;
    if (r.next(top) != mp::Status::Ok || top.kind != mp::Kind::Map || top.len != 4) return false;
    bool hw = false, hh = false, hc = false, hpx = false;
    for (uint32_t i = 0; i < 4; ++i) {
        mp::Element k;
        if (r.next(k) != mp::Status::Ok || k.kind != mp::Kind::Str) return false;
        std::string_view key((const char*)k.data, k.len);
        mp::Element v;
        if (r.next(v) != mp::Status::Ok) return false;
        if      (key == "w"  && v.kind == mp::Kind::Int) { w = (int32_t)v.i; hw = true; }
        else if (key == "h"  && v.kind == mp::Kind::Int) { h = (int32_t)v.i; hh = true; }
        else if (key == "c"  && v.kind == mp::Kind::Int) { c = (int32_t)v.i; hc = true; }
        else if (key == "px" && v.kind == mp::Kind::Bin) { px = v.data; px_len = v.len; hpx = true; }
        else return false;   // any other key/type -> not an image descriptor
    }
    return hw && hh && hc && hpx;
}

}  // namespace load_detail

// Read a canonical XEX1-v2 dump (magic 'XEX1' + msgpack body) back into a sealed
// pack. See the header banner for the untrusted-disk discipline.
inline LoadResult load_pack_v2(const uint8_t* data, size_t size) {
    LoadResult out;
    if (size < 5 || data[0] != 'X' || data[1] != 'E' || data[2] != 'X' || data[3] != '1') {
        out.error = "bad magic (not an XEX1 frame)";
        return out;
    }

    // Untrusted -> trusted canonical, through the ONE blessed edge (rejects a
    // forged pool-handle ext, non-string / duplicate keys, depth bombs, truncation).
    ingress::Result ing = ingress::canonicalize_entry(
        std::span<const uint8_t>(data + 4, size - 4), "xex1.v2");
    if (!ing.ok()) {
        out.error = std::string("ingress rejected dump: ") + mp::status_str(ing.codec_status);
        return out;
    }
    const mp::Bytes& body = ing.canonical;

    mp::Reader r(body);
    mp::Element top;
    if (r.next(top) != mp::Status::Ok || top.kind != mp::Kind::Map) {
        out.error = "frame body is not a map";
        return out;
    }

    xi::PackBuilder b;
    bool saw_v = false, saw_frame = false;
    for (uint32_t i = 0; i < top.len; ++i) {
        mp::Element k;
        if (r.next(k) != mp::Status::Ok || k.kind != mp::Kind::Str) { out.error = "bad top-level key"; return out; }
        std::string_view key((const char*)k.data, k.len);

        if (key == "v") {
            mp::Element v;
            if (r.next(v) != mp::Status::Ok) { out.error = "bad v field"; return out; }
            if (v.kind != mp::Kind::Int || v.i != 2) { out.error = "unsupported frame version"; return out; }
            saw_v = true;
        } else if (key == "channel") {
            mp::Element v;
            if (r.next(v) != mp::Status::Ok || v.kind != mp::Kind::Str) { out.error = "bad channel field"; return out; }
            out.channel.assign((const char*)v.data, v.len);
        } else if (key == "seq") {
            mp::Element v;
            if (r.next(v) != mp::Status::Ok) { out.error = "bad seq field"; return out; }
            if (v.kind == mp::Kind::Int)       out.seq = (uint64_t)v.i;
            else if (v.kind == mp::Kind::UInt) out.seq = v.u;
            else { out.error = "bad seq type"; return out; }
        } else if (key == "frame") {
            mp::Element fm;
            if (r.next(fm) != mp::Status::Ok || fm.kind != mp::Kind::Map) { out.error = "frame field is not a map"; return out; }
            for (uint32_t j = 0; j < fm.len; ++j) {
                mp::Element fk;
                if (r.next(fk) != mp::Status::Ok || fk.kind != mp::Kind::Str) { out.error = "bad entry key"; return out; }
                std::string entry_key((const char*)fk.data, fk.len);

                // Measure the value's full canonical span, then classify by its head.
                const size_t start = r.offset();
                if (load_detail::skip_value(r) != mp::Status::Ok) { out.error = "bad entry value"; return out; }
                const size_t end = r.offset();
                const uint8_t* vp = body.data() + start;
                const size_t   vn = end - start;

                mp::Reader hr(vp, vn);
                mp::Element head;
                hr.next(head);
                switch (head.kind) {
                    case mp::Kind::Int:   b.add_i64(entry_key, head.i); break;
                    case mp::Kind::UInt:  b.add_i64(entry_key, (int64_t)head.u); break;
                    case mp::Kind::Float: b.add_f64(entry_key, head.d); break;
                    case mp::Kind::Str:   b.add_str(entry_key, std::string_view((const char*)head.data, head.len)); break;
                    case mp::Kind::Bin:   b.add_bin(entry_key, head.data, head.len); break;
                    case mp::Kind::Map: {
                        int32_t w = 0, h = 0, c = 0; const uint8_t* px = nullptr; uint32_t pl = 0;
                        if (load_detail::as_image(vp, vn, w, h, c, px, pl))
                            b.add_image(entry_key, w, h, c, px);
                        else
                            b.add_mp(entry_key, vp, vn);   // nested msgpack (already through ingress)
                        break;
                    }
                    // Array / nil / bool: keep lossless as opaque canonical msgpack.
                    default: b.add_mp(entry_key, vp, vn); break;
                }
            }
            saw_frame = true;
        } else {
            // Unknown top-level key (forward-compat) — skip its value.
            if (load_detail::skip_value(r) != mp::Status::Ok) { out.error = "bad extension field"; return out; }
        }
    }
    if (!saw_v)     { out.error = "missing v field"; return out; }
    if (!saw_frame) { out.error = "missing frame field"; return out; }

    out.pack = b.seal();
    out.ok = true;
    return out;
}

// File convenience: read the whole dump file and load it. Missing/unreadable file
// is a !ok() Result, not a throw.
inline LoadResult load_pack_v2_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { LoadResult r; r.error = "cannot open file: " + path; return r; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    return load_pack_v2(bytes.data(), bytes.size());
}

}  // namespace xex1
}  // namespace xi

#endif  // XI_XEX1_PACK_LOAD_HPP
