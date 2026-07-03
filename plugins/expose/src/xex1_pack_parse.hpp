// xex1_pack_parse.hpp — parse a canonical XEX1-v3 dump into typed entry spans,
// WITHOUT building a host pack. This is the plugin-safe half of replay.
//
// Split rationale (doc 10 gate P3). Reading a dump back has two distinct steps:
//
//   1. PARSE (this header): magic + version check, the untrusted-disk ingress
//      edge, and the tagged entry walk — pure byte work over xi_mp/xi_ingress.
//      Safe to compile into a PLUGIN DLL (it mints no pool handles and touches
//      no host singleton), so a replay SOURCE plugin can parse a file and
//      rebuild the pack through the host xi.pack@1 builder (PackOut).
//   2. BUILD (xex1_pack_load.hpp): ParsedFrame -> sealed xi::Pack via
//      xi::PackBuilder. HOST-side only — a plugin must not build a host pack
//      directly (PackRegistry/ImagePool are per-module singletons).
//
// DISK IS UNTRUSTED (doc 07 "Ingress"). A dump file may be truncated,
// hand-forged, or a foreign artifact. The body does NOT go straight to the
// consumer — it goes through xi::ingress::canonicalize_entry, the ONE blessed
// edge: bounded nesting, declared-vs-actual length checks, string-keyed +
// dup-key rejection, and a forged pool-handle ext is refused. Every span this
// parser hands out points into the POST-ingress canonical body.
//
// FORMAT (XEX1-v3, the finalized canonical dump — xex1_encode.hpp):
//   'XEX1' + { "v":3, "channel":<str>, "seq":<int>,
//              "frame":{ <key>:[<tag>, <value>], ... } }
// Every entry carries its XI_PACK_TAG_* on the wire, so the entry type is
// RECOVERED EXACTLY — never guessed by shape (the v2 draft's image-descriptor
// ambiguity is unrepresentable). Tag/value agreement is ENFORCED here: a tag
// that contradicts its value's msgpack kind is a forged/corrupt file and the
// whole parse is refused (fail loud, no partial frames).
#ifndef XI_XEX1_PACK_PARSE_HPP
#define XI_XEX1_PACK_PARSE_HPP

#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include <xi/xi_abi.h>        // XI_PACK_TAG_* (the entry tag vocabulary)
#include <xi/xi_ingress.hpp>  // xi::ingress::canonicalize_entry (the untrusted edge)
#include <xi/xi_mp.hpp>       // xi::mp::Reader (decode the trusted canonical body)

namespace xi {
namespace xex1 {

// One parsed entry. Spans are (offset,len) into ParsedFrame::body — valid for
// the ParsedFrame's lifetime, zero further copies until the pack is built.
struct ParsedEntry {
    std::string key;
    uint8_t     tag = 0;              // XI_PACK_TAG_* (from the wire, enforced)
    // Non-image: the entry's complete canonical value bytes.
    size_t off = 0, len = 0;
    // Image (tag == XI_PACK_TAG_IMAGE): dims + raw pixel span.
    int32_t w = 0, h = 0, c = 0;
    size_t px_off = 0; uint32_t px_len = 0;
};

// The outcome of a parse. Meaningful only when ok(); `body` OWNS the canonical
// bytes every entry span points into.
struct ParsedFrame {
    bool        ok = false;
    std::string error;        // human reason when !ok
    std::string channel;      // lifted frame-header fields
    uint64_t    seq = 0;
    mp::Bytes   body;         // POST-ingress canonical frame body
    std::vector<ParsedEntry> entries;

    explicit operator bool() const { return ok; }
    const uint8_t* at(size_t off) const { return body.data() + off; }
};

namespace parse_detail {

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

// Decode the canonical value at [p,p+n) as an image descriptor map {w,h,c,px}.
// Returns false when the shape is anything else (the caller then REFUSES the
// frame — the tag SAID image, so a non-descriptor value is forged/corrupt).
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

}  // namespace parse_detail

// Parse a canonical XEX1-v3 dump (magic 'XEX1' + msgpack body). See the header
// banner for the untrusted-disk discipline. Every failure is a refusal of the
// WHOLE frame with a human reason — no partial results.
inline ParsedFrame parse_frame_v3(const uint8_t* data, size_t size) {
    ParsedFrame out;
    if (size < 5 || data[0] != 'X' || data[1] != 'E' || data[2] != 'X' || data[3] != '1') {
        out.error = "bad magic (not an XEX1 frame)";
        return out;
    }

    // Untrusted -> trusted canonical, through the ONE blessed edge (rejects a
    // forged pool-handle ext, non-string / duplicate keys, depth bombs, truncation).
    ingress::Result ing = ingress::canonicalize_entry(
        std::span<const uint8_t>(data + 4, size - 4), "xex1.v3");
    if (!ing.ok()) {
        out.error = std::string("ingress rejected dump: ") + mp::status_str(ing.codec_status);
        return out;
    }
    out.body = std::move(ing.canonical);
    const mp::Bytes& body = out.body;

    mp::Reader r(body);
    mp::Element top;
    if (r.next(top) != mp::Status::Ok || top.kind != mp::Kind::Map) {
        out.error = "frame body is not a map";
        return out;
    }

    bool saw_v = false, saw_frame = false;
    for (uint32_t i = 0; i < top.len; ++i) {
        mp::Element k;
        if (r.next(k) != mp::Status::Ok || k.kind != mp::Kind::Str) { out.error = "bad top-level key"; return out; }
        std::string_view key((const char*)k.data, k.len);

        if (key == "v") {
            mp::Element v;
            if (r.next(v) != mp::Status::Ok) { out.error = "bad v field"; return out; }
            if (v.kind != mp::Kind::Int || v.i != 3) {
                // The v2 DRAFT (tagless entries) is refused here by design: its
                // image-vs-map ambiguity cannot be resolved without guessing.
                // See docs/new_gen/13-replay-file-migration.md.
                out.error = "unsupported frame version (expected v3, the finalized tagged dump)";
                return out;
            }
            saw_v = true;
        } else if (key == "channel") {
            mp::Element v;
            if (r.next(v) != mp::Status::Ok || v.kind != mp::Kind::Str) { out.error = "bad channel field"; return out; }
            out.channel.assign((const char*)v.data, v.len);
        } else if (key == "seq") {
            mp::Element v;
            if (r.next(v) != mp::Status::Ok) { out.error = "bad seq field"; return out; }
            if (v.kind == mp::Kind::Int && v.i >= 0) out.seq = (uint64_t)v.i;
            else if (v.kind == mp::Kind::UInt)       out.seq = v.u;
            else { out.error = "bad seq type"; return out; }
        } else if (key == "frame") {
            mp::Element fm;
            if (r.next(fm) != mp::Status::Ok || fm.kind != mp::Kind::Map) { out.error = "frame field is not a map"; return out; }
            out.entries.reserve(fm.len);
            for (uint32_t j = 0; j < fm.len; ++j) {
                mp::Element fk;
                if (r.next(fk) != mp::Status::Ok || fk.kind != mp::Kind::Str) { out.error = "bad entry key"; return out; }
                ParsedEntry pe;
                pe.key.assign((const char*)fk.data, fk.len);

                // [tag, value] — the per-entry type tag (v3's whole point).
                mp::Element arr;
                if (r.next(arr) != mp::Status::Ok || arr.kind != mp::Kind::Array || arr.len != 2) {
                    out.error = "entry '" + pe.key + "' is not a [tag, value] pair";
                    return out;
                }
                mp::Element tg;
                if (r.next(tg) != mp::Status::Ok || tg.kind != mp::Kind::Int || tg.i < 0 || tg.i > 255) {
                    out.error = "entry '" + pe.key + "' has a bad tag";
                    return out;
                }
                pe.tag = (uint8_t)tg.i;

                // Measure the value's full canonical span.
                const size_t start = r.offset();
                if (parse_detail::skip_value(r) != mp::Status::Ok) { out.error = "bad entry value"; return out; }
                const size_t end = r.offset();
                const uint8_t* vp = body.data() + start;
                const size_t   vn = end - start;
                pe.off = start; pe.len = vn;

                // ENFORCE tag/value agreement — a contradiction is a forged or
                // corrupt file, refused whole (fail loud, doc 07 ingress spirit).
                mp::Reader hr(vp, vn);
                mp::Element head;
                hr.next(head);
                bool agree = false;
                switch (pe.tag) {
                    case XI_PACK_TAG_I64:
                        agree = (head.kind == mp::Kind::Int || head.kind == mp::Kind::UInt); break;
                    case XI_PACK_TAG_F64: agree = (head.kind == mp::Kind::Float); break;
                    case XI_PACK_TAG_BOOL: agree = (head.kind == mp::Kind::Bool); break;
                    case XI_PACK_TAG_STR: agree = (head.kind == mp::Kind::Str);   break;
                    case XI_PACK_TAG_BIN: agree = (head.kind == mp::Kind::Bin);   break;
                    case XI_PACK_TAG_MP:  agree = true; break;   // opaque: any one canonical value
                    case XI_PACK_TAG_IMAGE: {
                        const uint8_t* px = nullptr;
                        if (!parse_detail::as_image(vp, vn, pe.w, pe.h, pe.c, px, pe.px_len)) break;
                        // Dim sanity on untrusted input: positive dims whose
                        // product IS the pixel payload — a forged {w,h,c} larger
                        // than px would otherwise make the pack builder read out
                        // of bounds.
                        if (pe.w <= 0 || pe.h <= 0 || pe.c <= 0) break;
                        const uint64_t need = (uint64_t)pe.w * (uint64_t)pe.h * (uint64_t)pe.c;
                        if (need != (uint64_t)pe.px_len) break;
                        pe.px_off = (size_t)(px - body.data());
                        agree = true;
                        break;
                    }
                    default: break;   // unknown tag in a v3 frame: forged/corrupt
                }
                if (!agree) {
                    out.error = "entry '" + pe.key + "' tag contradicts its value (forged or corrupt dump)";
                    return out;
                }
                out.entries.push_back(std::move(pe));
            }
            saw_frame = true;
        } else {
            // Unknown top-level key (forward-compat) — skip its value.
            if (parse_detail::skip_value(r) != mp::Status::Ok) { out.error = "bad extension field"; return out; }
        }
    }
    if (!saw_v)     { out.error = "missing v field"; return out; }
    if (!saw_frame) { out.error = "missing frame field"; return out; }

    out.ok = true;
    return out;
}

// File convenience: read the whole dump file and parse it. Missing/unreadable
// file is a !ok() result, not a throw.
inline ParsedFrame parse_frame_v3_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { ParsedFrame r; r.error = "cannot open file: " + path; return r; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    return parse_frame_v3(bytes.data(), bytes.size());
}

}  // namespace xex1
}  // namespace xi

#endif  // XI_XEX1_PACK_PARSE_HPP
