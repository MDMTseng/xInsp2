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
#include <xi/xi_blob_head.hpp> // xi::blob_head_validate — the ONE blob head seam (plugin-safe, spec 30)
#include <xi/xi_ingress.hpp>  // xi::ingress::canonicalize_entry (the untrusted edge)
#include <xi/xi_mp.hpp>       // xi::mp::Reader (decode the trusted canonical body)

namespace xi {
namespace xex1 {

// One parsed entry. Spans are (offset,len) into ParsedFrame::body — valid for
// the ParsedFrame's lifetime, zero further copies until the pack is built.
struct ParsedEntry {
    std::string key;
    uint8_t     tag = 0;              // XI_PACK_TAG_* (from the wire, enforced)
    // Scalar/str/bin/mp: the entry's complete canonical value bytes.
    size_t off = 0, len = 0;
    // Blob (tag == XI_PACK_TAG_BLOB, spec 30): the ENTIRE self-describing buffer
    // (magic + desc_len + descriptor + pad + payload) as a span into body — the
    // verbatim bin payload, already blob_head_validate'd.
    size_t   blob_off = 0;
    uint32_t blob_len = 0;
};

// The outcome of a parse. Meaningful only when ok(); `body` OWNS the canonical
// bytes every entry span points into.
struct ParsedFrame {
    bool        ok = false;
    std::string error;        // human reason when !ok
    std::string channel;      // lifted frame-header fields
    uint64_t    seq = 0;
    // F5: presence, not just value — the v3 header's channel/seq are OPTIONAL.
    // A consumer (record_replay) must inject $channel/$seq back onto the rebuilt
    // pack ONLY when the file carried them, else a pack that never had them gains
    // a spurious ""/0 pair on replay. Distinguishes "absent" from "present but
    // empty/zero" (which channel==""/seq==0 alone cannot).
    bool        has_channel = false;
    bool        has_seq = false;
    mp::Bytes   body;         // POST-ingress canonical frame body
    std::vector<ParsedEntry> entries;

    explicit operator bool() const { return ok; }
    const uint8_t* at(size_t off) const { return body.data() + off; }
};

namespace parse_detail {

// Consume ONE complete msgpack value (header + full body) at the reader cursor.
// Used to measure a frame value's canonical byte span [start,end).
//
// SELF-CONTAINED depth guard: this walk recurses per nesting level, so it
// carries its OWN bound (mirroring the reader's mp::kDefaultMaxDepth = 64)
// instead of relying on ingress::canonicalize_entry having already rejected
// depth bombs upstream. That coupling was implicit — a refactor that calls
// skip_value before ingress, or raises the ingress max_depth, must not turn a
// hostile deeply-nested .xex1 into a stack-overflow DoS here.
inline mp::Status skip_value(mp::Reader& r, int depth = 0) {
    if (depth >= mp::kDefaultMaxDepth) return mp::Status::DepthExceeded;
    mp::Element e;
    mp::Status s = r.next(e);
    if (s != mp::Status::Ok) return s;
    if (e.kind == mp::Kind::Array) {
        for (uint32_t i = 0; i < e.len; ++i) { s = skip_value(r, depth + 1); if (s != mp::Status::Ok) return s; }
    } else if (e.kind == mp::Kind::Map) {
        for (uint32_t i = 0; i < e.len; ++i) {
            s = skip_value(r, depth + 1); if (s != mp::Status::Ok) return s;  // key
            s = skip_value(r, depth + 1); if (s != mp::Status::Ok) return s;  // value
        }
    }
    return mp::Status::Ok;
}

// The host ImagePool caps a single pool buffer at 1 GiB (ImagePool::create,
// xi_image_pool.hpp: `pixels > (int64_t(1) << 30)` -> handle 0). A blob's whole
// self-describing buffer is minted through that pool, so mirror the bound here —
// this plugin-safe header deliberately includes no host pool header, so the
// value is restated; KEEP IN SYNC. Without this check a 1-4 GiB blob entry
// passes the bin (uint32) length validation, then the rebuild's mint returns
// null and the frame silently loses the entry. Refuse it at parse, loud.
constexpr uint64_t kPoolMaxBytes = uint64_t(1) << 30;

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
            out.has_channel = true;
        } else if (key == "seq") {
            mp::Element v;
            if (r.next(v) != mp::Status::Ok) { out.error = "bad seq field"; return out; }
            if (v.kind == mp::Kind::Int && v.i >= 0) out.seq = (uint64_t)v.i;
            else if (v.kind == mp::Kind::UInt)       out.seq = v.u;
            else { out.error = "bad seq type"; return out; }
            out.has_seq = true;
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
                    case XI_PACK_TAG_BLOB: {
                        // A persisted blob is the ENTIRE self-describing buffer as
                        // one `bin` (spec 30 — memory == wire). The value MUST be a
                        // bin whose bytes pass the ONE core head seam
                        // (blob_head_validate): magic 'XBD1', desc_len in bounds, a
                        // CANONICAL descriptor map, payload_off <= len. A forged /
                        // corrupt blob is refused whole (fail loud), exactly like
                        // every other bad entry. (ingress canonicalized the FRAME,
                        // but a bin's bytes are opaque to msgpack — the descriptor
                        // inside is validated only HERE.)
                        if (head.kind != mp::Kind::Bin) break;
                        if (!xi::blob_head_validate(head.data, head.len)) break;
                        // Align with the host pool's 1 GiB per-buffer cap (see
                        // kPoolMaxBytes): an oversize blob must fail HERE, not
                        // decode to a failed mint on rebuild. Distinct message.
                        if ((uint64_t)head.len > parse_detail::kPoolMaxBytes) {
                            out.error = "entry '" + pe.key +
                                        "' blob exceeds the 1 GiB pool cap (" +
                                        std::to_string(head.len) + " bytes)";
                            return out;
                        }
                        pe.blob_off = (size_t)(head.data - body.data());
                        pe.blob_len = head.len;
                        agree = true;
                        break;
                    }
                    default: break;   // unknown tag (incl. the retired IMAGE=4): forged/corrupt
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

// File convenience: read the whole dump file and parse it. Missing/unreadable/
// oversized file, or an allocation failure, is a !ok() result — NEVER a throw.
// The caller (record_replay) turns a !ok result into a sealed $fault; an escaping
// exception would unwind PAST that fail-loud emit and be swallowed at the pack
// door, so the untrusted read is size-capped and exception-contained here.
inline ParsedFrame parse_frame_v3_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { ParsedFrame r; r.error = "cannot open file: " + path; return r; }
    const std::streamoff size = in.tellg();
    if (size < 0) { ParsedFrame r; r.error = "cannot size file: " + path; return r; }
    // Refuse a pathologically large dump BEFORE slurping it into RAM — an
    // unbounded read of an untrusted file is an OOM DoS. A generous ceiling (4×
    // the per-blob pool cap) admits any legitimate single frame.
    constexpr uint64_t kMaxReplayFileBytes = parse_detail::kPoolMaxBytes * 4;  // 4 GiB
    if (static_cast<uint64_t>(size) > kMaxReplayFileBytes) {
        ParsedFrame r; r.error = "replay file too large: " + path; return r;
    }
    in.seekg(0, std::ios::beg);
    try {
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (size > 0) in.read(reinterpret_cast<char*>(bytes.data()), size);
        return parse_frame_v3(bytes.data(), bytes.size());
    } catch (...) {
        ParsedFrame r; r.error = "cannot read replay file (alloc/io): " + path; return r;
    }
}

}  // namespace xex1
}  // namespace xi

#endif  // XI_XEX1_PACK_PARSE_HPP
