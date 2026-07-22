// test_xex1_fixtures.cpp — generate + verify the XEX1 binary golden fixtures.
//
// The XEX1 frame is encoded by ONE C++ encoder (xex1_encode.hpp) and decoded by
// two independent hand-rolled msgpack readers — the expose plugin's webUI
// (toolbox/expose/ui/index.html) and qa/lib/xex1.py. Nothing tested that
// the three agree. This tool is the C++ leg: it emits a set of golden .bin frames
// straight from the production encoder and byte-compares the encoder's output
// against the committed goldens on every run, so the fixtures are reproducible and
// the encoder can never silently change the wire bytes. The JS and Python decoder
// tests (ui-components/test/xex1-golden.mjs, tools/xinsp2_py/tests/test_xex1_frames.py)
// decode the SAME .bin files and assert the manifest content — that closes the
// cross-implementation loop.
//
// Modes:
//   test_xex1_fixtures              (ctest default) — VERIFY: re-encode every frame
//                                    and byte-compare against protocol/fixtures/binary/*.bin;
//                                    fail if any differs or is missing.
//   test_xex1_fixtures --regen      REGENERATE: (re)write the .bin files + manifest.json.
//                                    Run this after intentionally changing a frame.
//
// Regeneration command (from the repo root, after a Release build):
//   ctest --test-dir build -R xex1_fixtures            # verify
//   ./build/plugins/test_xex1_fixtures --regen         # rewrite goldens (exact path per generator)
//
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <cstring>

#include <xi/xi_b64.hpp>
#include <xi/xi_blob_head.hpp>   // blob head format: kBlobMagic / blob_payload_off / put_u32_le
#include <xi/xi_json.hpp>
#include <xi/xi_mp.hpp>

#include "xex1_encode.hpp"

using xi::xex1::EncImage;

namespace {

// Build the canonical descriptor map for an xi/image blob: {t,w,h,c,dt}.
std::vector<uint8_t> image_desc(int w, int h, int c, const char* dt) {
    xi::mp::Writer d;
    d.map(5);
    d.key("t");  d.str("xi/image");
    d.key("w");  d.int_(w);
    d.key("h");  d.int_(h);
    d.key("c");  d.int_(c);
    d.key("dt"); d.str(dt);
    return d.take();
}

// Assemble a self-describing blob buffer (spec 30): magic 'XBD1' (LE) +
// desc_len (LE) + descriptor + zero pad to the 64B payload alignment + payload.
// This is byte-identical to what xi::mint_blob writes host-side.
std::vector<uint8_t> make_blob_buffer(const std::vector<uint8_t>& desc,
                                      const std::vector<uint8_t>& payload) {
    const uint32_t desc_len = (uint32_t)desc.size();
    const uint64_t payload_off = xi::blob_payload_off(desc_len);
    std::vector<uint8_t> buf((size_t)payload_off + payload.size(), 0);  // pad = zero
    xi::pack_mp_detail::put_u32_le(buf.data() + 0, xi::kBlobMagic);
    xi::pack_mp_detail::put_u32_le(buf.data() + 4, desc_len);
    if (desc_len) std::memcpy(buf.data() + 8, desc.data(), desc_len);
    if (!payload.empty()) std::memcpy(buf.data() + (size_t)payload_off, payload.data(), payload.size());
    return buf;
}

// Deterministic synthetic "JPEG" payload of a given size — content is irrelevant
// to the codec (images ride as opaque msgpack bin); the pattern just makes each
// image distinguishable so a decoder that mis-slices a bin blob is caught.
std::vector<uint8_t> synth(size_t n, uint8_t salt) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)((i * 37 + salt) & 0xFF);
    return v;
}

// Routed through the shared SDK leaf (xi/xi_b64.hpp). The independence this
// test guards is the XEX1 msgpack ENCODER vs the JS/Python decoders — base64
// here only serializes manifest bytes (a wrong encoding would fail the
// cross-implementation manifest check loudly), so sharing the leaf is safe.
std::string b64(const std::vector<uint8_t>& in) { return xi::b64_encode(in.data(), in.size()); }

struct Frame {
    std::string                                   name;      // -> <name>.bin
    std::string                                   channel;
    uint64_t                                      seq = 0;
    std::string                                   json;
    std::vector<EncImage>                         images;
    std::vector<std::pair<std::string, uint64_t>> extra;
    std::string                                   note;
};

// The full fixture set. Every msgpack width the encoder can emit gets a boundary
// frame, so a decoder missing one fails against a golden here.
std::vector<Frame> build_frames() {
    std::vector<Frame> fs;

    fs.push_back({"minimal", "c", 0, "{}", {}, {},
        "smallest valid frame: fixmap(5), fixstr, fixint 0, empty fixarray"});

    fs.push_back({"realistic", "line1/cam0", 42,
        R"({"width":640,"height":480,"defect":false,"score":0.97})",
        {{"edges", synth(200, 'e')}, {"mask", synth(120, 'm')}}, {},
        "typical multi-field frame: str8 json, two bin8 images, fixarray(2)"});

    fs.push_back({"nonfinite", "m", 3,
        R"({"a":"NaN","b":"Infinity","c":"-Infinity","d":1.5})", {}, {},
        "non-finite sentinels the encoder emits as JSON strings; decoders restore to NaN/Inf"});

    {
        std::string chan(40, 'x');                 // 40 > 31 -> str8 channel
        std::string blob(270, 'z');                // -> json body > 255 -> str16
        fs.push_back({"str16", chan, 1,
            std::string("{\"blob\":\"") + blob + "\"}", {}, {},
            "str8 channel + str16 json string (width boundary crossings)"});
    }

    fs.push_back({"uint8",  "c", 200,        "{}", {}, {}, "seq 200 -> msgpack uint8 (0xCC)"});
    fs.push_back({"uint16", "c", 1000,       "{}", {}, {}, "seq 1000 -> msgpack uint16 (0xCD)"});
    fs.push_back({"uint32", "c", 70000,      "{}", {}, {}, "seq 70000 -> msgpack uint32 (0xCE)"});
    fs.push_back({"uint64", "c", 4294967296ull, "{}", {}, {}, "seq 2^32 -> msgpack uint64 (0xCF)"});

    fs.push_back({"bin16", "c", 1,  "{}", {{"big", synth(300, 'b')}}, {},
        "300-byte image -> msgpack bin16 (0xC5)"});

    {
        std::vector<EncImage> imgs;
        for (int i = 0; i < 17; ++i) {
            char key[8]; std::snprintf(key, sizeof key, "img%02d", i);
            imgs.push_back({key, synth(4, (uint8_t)('a' + i))});
        }
        fs.push_back({"array16", "c", 2, "{}", std::move(imgs), {},
            "17 images -> images[] crosses fixarray into array16 (0xDC)"});
    }

    {
        std::vector<std::pair<std::string, uint64_t>> extra;
        for (int i = 0; i < 12; ++i) {
            char key[8]; std::snprintf(key, sizeof key, "ext%02d", i);
            extra.emplace_back(key, (uint64_t)(100 + i));
        }
        // 5 canonical + 12 extension = 17 top-level keys -> top map is map16 (0xDE).
        // This is the fixmap-only-cap regression golden: a decoder that handles only
        // fixmap throws on the 0xDE byte and CANNOT read this frame.
        fs.push_back({"map16", "c", 9, "{}", {}, std::move(extra),
            "17 top-level keys -> map16 (0xDE); the fixmap-cap regression fixture"});
    }

    return fs;
}

std::vector<uint8_t> encode(const Frame& f) {
    return xi::xex1::encode_frame(f.channel, f.seq, f.json, f.images, f.extra);
}

// --- XEX1-v3 (the canonical frame dump) fixtures ----------------------------
//
// A v3 golden is a channel/seq + a generic entry list. Each field carries its
// value as a decoder-visible expectation AND is encoded into a V3Entry with its
// canonical msgpack bytes (scalars/str/bin via xi::mp::Writer, nested msgpack
// verbatim, and BLOBS — spec 30 — as the ENTIRE self-describing buffer inlined
// in one bin) — the exact bytes the expose pack door emits for the same logical
// frame. The JS + Python decoders decode these same .bin files and assert the
// field values below.

struct V3Field {
    std::string          key;
    std::string          kind;   // "i64" | "f64" | "bool" | "str" | "bin" | "mp" | "blob"
    int64_t              i = 0;
    double               f = 0;
    bool                 b = false;   // bool value
    std::string          s;      // str value
    std::vector<uint8_t> bytes;  // bin payload / mp canonical bytes / blob PAYLOAD
    xi::Json             mp_value = xi::Json();  // decoded expectation for an mp field
    // Blob (kind == "blob", spec 30): `desc` is the canonical descriptor map
    // bytes, `bytes` (above) is the payload. `t` is the descriptor's "t" type
    // string (a decoder-visible expectation); w/h/c/dt are set for xi/image blobs.
    std::vector<uint8_t> desc;
    std::string          t;
    std::string          dt;
    int32_t              w = 0, h = 0, c = 0;   // xi/image dims (from the descriptor)
    // WS-preview arm (kind == "preview"): the encoder's WS-SEND substitution for
    // an xi/image blob. `w/h/c` are the source dims, `bytes` is the JPEG, `pv_q`
    // the quality — emitted as { preview:{w,h,c,enc:"jpeg",q,data} }. WS-only
    // (never persisted), but the codec is exercised here so all 3 legs guard it.
    int32_t              pv_q = 0;
};

struct FrameV3 {
    std::string          name;
    std::string          channel;
    uint64_t             seq = 0;
    std::vector<V3Field> fields;
    std::string          note;
};

// Encode one FrameV3 into wire bytes through the SHARED v3 encoder. Blob buffers
// are assembled here into `blob_store`, which must outlive encode_frame_v3 (the
// V3Entry.blob pointers borrow into it) — hence built up front, never realloc'd.
std::vector<uint8_t> encode_v3(const FrameV3& f) {
    std::vector<xi::xex1::V3Entry> entries;
    entries.reserve(f.fields.size());
    std::vector<std::vector<uint8_t>> blob_store;
    blob_store.reserve(f.fields.size());
    for (const auto& fld : f.fields) {
        xi::xex1::V3Entry e;
        e.key = fld.key;
        if (fld.kind == "i64") {
            e.tag = XI_PACK_TAG_I64; xi::mp::Writer w; w.int_(fld.i); e.value = w.take();
        } else if (fld.kind == "f64") {
            e.tag = XI_PACK_TAG_F64; xi::mp::Writer w; w.float_(fld.f); e.value = w.take();
        } else if (fld.kind == "bool") {
            e.tag = XI_PACK_TAG_BOOL; xi::mp::Writer w; w.boolean(fld.b); e.value = w.take();
        } else if (fld.kind == "str") {
            e.tag = XI_PACK_TAG_STR; xi::mp::Writer w; w.str(fld.s); e.value = w.take();
        } else if (fld.kind == "bin") {
            e.tag = XI_PACK_TAG_BIN; xi::mp::Writer w; w.bin(fld.bytes.data(), fld.bytes.size());
            e.value = w.take();
        } else if (fld.kind == "mp") {
            e.tag = XI_PACK_TAG_MP; e.value = fld.bytes;   // already-canonical nested bytes
        } else if (fld.kind == "blob") {
            e.tag = XI_PACK_TAG_BLOB;
            blob_store.push_back(make_blob_buffer(fld.desc, fld.bytes));
            const std::vector<uint8_t>& buf = blob_store.back();
            e.blob = buf.data(); e.blob_len = buf.size();
        } else if (fld.kind == "preview") {
            // WS-preview arm: { preview:{w,h,c,enc:"jpeg",q,data} } (bytes = jpeg).
            e.tag = XI_PACK_TAG_BLOB;
            e.preview = true;
            e.pv_w = fld.w; e.pv_h = fld.h; e.pv_c = fld.c; e.pv_q = fld.pv_q;
            e.pv_jpeg = fld.bytes.data(); e.pv_len = fld.bytes.size();
        }
        entries.push_back(std::move(e));
    }
    return xi::xex1::encode_frame_v3(f.channel, f.seq, entries);
}

std::vector<FrameV3> build_frames_v3() {
    std::vector<FrameV3> fs;

    fs.push_back({"v3_minimal", "c", 0, {}, "empty frame map — smallest v3 dump"});

    {
        FrameV3 f; f.name = "v3_scalars"; f.channel = "line1/cam0"; f.seq = 7;
        f.fields.push_back({"count", "i64", 42});
        f.fields.push_back({"neg", "i64", -5});
        f.fields.push_back({"big", "i64", 4294967296LL});      // 2^32 — still int64 0xd3
        {V3Field fl; fl.key = "score"; fl.kind = "f64"; fl.f = 1.5; f.fields.push_back(fl);}
        {V3Field fl; fl.key = "label"; fl.kind = "str"; fl.s = "ok"; f.fields.push_back(fl);}
        f.note = "scalar/str entries, canonical int64/float64/str32 verbatim on the wire";
        fs.push_back(std::move(f));
    }

    {
        // An xi/image self-describing blob (spec 30): the descriptor
        // {"t":"xi/image","w":2,"h":2,"c":1,"dt":"u8"} + 4 raw u8 pixels, inlined
        // as the ENTIRE buffer (magic+desc_len+desc+pad+payload) in one bin.
        FrameV3 f; f.name = "v3_blob_image"; f.channel = "c"; f.seq = 3;
        V3Field img; img.key = "frame"; img.kind = "blob";
        img.t = "xi/image"; img.dt = "u8"; img.w = 2; img.h = 2; img.c = 1;
        img.desc  = image_desc(2, 2, 1, "u8");
        img.bytes = {10, 20, 30, 40};   // raw pixels (the payload)
        f.fields.push_back(std::move(img));
        f.note = "xi/image blob: self-describing buffer (head+descriptor+payload) inlined as one bin (spec 30)";
        fs.push_back(std::move(f));
    }

    {
        // A generic (non-image) self-describing blob: a custom descriptor
        // {"t":"acme/roi","n":3} + arbitrary payload bytes. Exercises the
        // decoders' GENERIC blob path (head validation + descriptor decode) — the
        // core owns no type space, so "t" is convention only.
        FrameV3 f; f.name = "v3_blob_data"; f.channel = "c"; f.seq = 4;
        xi::mp::Writer d;
        d.map(2); d.key("t"); d.str("acme/roi"); d.key("n"); d.int_(3);
        V3Field blob; blob.key = "roi"; blob.kind = "blob";
        blob.t = "acme/roi"; blob.desc = d.take();
        blob.bytes = {0xAA, 0xBB, 0xCC};   // opaque payload
        f.fields.push_back(std::move(blob));
        f.note = "generic (non-image) blob: custom descriptor + opaque payload (self-describing, spec 30)";
        fs.push_back(std::move(f));
    }

    {
        // Bool entries (pack-plane hardening — tag XI_PACK_TAG_BOOL): the wire
        // value is the single canonical msgpack byte 0xc3/0xc2, spliced verbatim.
        FrameV3 f; f.name = "v3_bool"; f.channel = "c"; f.seq = 9;
        {V3Field fl; fl.key = "pass"; fl.kind = "bool"; fl.b = true;  f.fields.push_back(fl);}
        {V3Field fl; fl.key = "fail"; fl.kind = "bool"; fl.b = false; f.fields.push_back(fl);}
        f.note = "bool entries — canonical msgpack true/false (0xc3/0xc2) verbatim on the wire";
        fs.push_back(std::move(f));
    }

    {
        // A nested msgpack entry (blob_analysis-shaped): "blobs" = [ {area, cx} ].
        // Built with the SAME xi::mp::Writer a producer uses; the decoders decode
        // it into a native list/array and compare against mp_value.
        FrameV3 f; f.name = "v3_nested"; f.channel = "c"; f.seq = 1;
        xi::mp::Writer w;
        w.array(2);
        w.map(2); w.key("area"); w.int_(100); w.key("cx"); w.float_(2.5);
        w.map(2); w.key("area"); w.int_(250); w.key("cx"); w.float_(8.0);
        V3Field blobs; blobs.key = "blobs"; blobs.kind = "mp"; blobs.bytes = w.take();
        auto arr = xi::Json::array();
        auto b0 = xi::Json::object(); b0.set("area", (int64_t)100).set("cx", 2.5); arr.push(b0);
        auto b1 = xi::Json::object(); b1.set("area", (int64_t)250).set("cx", 8.0); arr.push(b1);
        blobs.mp_value = arr;
        f.fields.push_back(std::move(blobs));
        f.note = "nested canonical msgpack (array of maps) rides verbatim — doc 07 D3";
        fs.push_back(std::move(f));
    }

    {
        // The WS-preview arm (E2, spec 30): an xi/image blob whose raw payload is
        // substituted on the live wire by { preview:{w,h,c,enc:"jpeg",q,data} }.
        // WS-SEND-only (record_save never emits it), but pinned here so all three
        // decoder legs guard the preview shape — the regression that shipped
        // preview WITHOUT w/h/c would have failed this golden.
        FrameV3 f; f.name = "v3_preview"; f.channel = "c"; f.seq = 5;
        V3Field pv; pv.key = "frame"; pv.kind = "preview";
        pv.w = 4; pv.h = 2; pv.c = 1; pv.pv_q = 80;
        pv.bytes = synth(24, 'j');   // stand-in JPEG bytes (opaque to the codec)
        f.fields.push_back(std::move(pv));
        f.note = "WS-preview arm: { preview:{w,h,c,enc:jpeg,q,data} } — behavior-equivalent to the old IMAGE preview";
        fs.push_back(std::move(f));
    }

    return fs;
}

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::string binary_dir() {
    const char* fx = std::getenv("XINSP2_FIXTURES");
    std::string base = fx ? fx : "../../protocol/fixtures";
    return base + "/binary";
}

// Manifest entry for one v3 frame: channel/seq + the decoded field expectations.
xi::Json manifest_v3(const FrameV3& f) {
    xi::Json fj = xi::Json::object();
    fj.set("file", f.name + ".bin");
    fj.set("v", 3);
    fj.set("channel", f.channel);
    fj.set("seq", (int64_t)f.seq);
    xi::Json fields = xi::Json::array();
    for (const auto& fld : f.fields) {
        xi::Json j = xi::Json::object();
        j.set("key", fld.key);
        j.set("kind", fld.kind);
        if (fld.kind == "i64")        j.set("value", fld.i);
        else if (fld.kind == "f64")   j.set("value", fld.f);
        else if (fld.kind == "bool")  j.set("value", fld.b);
        else if (fld.kind == "str")   j.set("value", fld.s);
        else if (fld.kind == "bin") { j.set("b64", b64(fld.bytes)); j.set("size", (int64_t)fld.bytes.size()); }
        else if (fld.kind == "mp")    j.set("value", fld.mp_value);
        else if (fld.kind == "blob") {
            // A decoder extracts the whole buffer from the entry's bin, validates
            // the head (magic 'XBD1' + desc_len + payload_off), then byte-compares
            // the descriptor + payload against these, and decodes "t" from the desc.
            j.set("t", fld.t);
            j.set("desc_b64", b64(fld.desc));
            j.set("desc_size", (int64_t)fld.desc.size());
            j.set("payload_b64", b64(fld.bytes));
            j.set("payload_size", (int64_t)fld.bytes.size());
            if (fld.t == "xi/image") {
                j.set("w", fld.w); j.set("h", fld.h); j.set("c", fld.c); j.set("dt", fld.dt);
            }
        }
        else if (fld.kind == "preview") {
            // A decoder surfaces images[key]["preview"] = {w,h,c,enc,q,data}.
            j.set("w", fld.w); j.set("h", fld.h); j.set("c", fld.c);
            j.set("q", fld.pv_q); j.set("enc", std::string("jpeg"));
            j.set("data_b64", b64(fld.bytes));
            j.set("data_size", (int64_t)fld.bytes.size());
        }
        fields.push(j);
    }
    fj.set("fields", fields);
    fj.set("note", f.note);
    return fj;
}

int regen(const std::string& dir, const std::vector<Frame>& frames,
          const std::vector<FrameV3>& frames_v3) {
    xi::Json manifest = xi::Json::object();
    manifest.set("note",
        "XEX1 binary golden frames. Generated by toolbox/expose/tests/test_xex1_fixtures.cpp "
        "--regen from the production encoder (toolbox/expose/src/xex1_encode.hpp). Do not edit "
        "by hand. Each frame carries a `v` (1 = legacy display frame; 3 = canonical tagged frame "
        "dump). Each *.bin is decoded and checked against this manifest by "
        "ui-components/test/xex1-golden.mjs and tools/xinsp2_py/tests/test_xex1_frames.py.");
    xi::Json arr = xi::Json::array();

    for (const auto& f : frames) {
        std::vector<uint8_t> bytes = encode(f);
        std::string path = dir + "/" + f.name + ".bin";
        std::ofstream out(path, std::ios::binary);
        if (!out) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
        out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
        std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());

        xi::Json fj = xi::Json::object();
        fj.set("file", f.name + ".bin");
        fj.set("v", 1);
        fj.set("channel", f.channel);
        fj.set("seq", (int64_t)f.seq);        // <= 2^32 fits; the 2^63 case would need a string, none here
        fj.set("json", f.json);
        xi::Json imgs = xi::Json::array();
        for (const auto& im : f.images) {
            xi::Json ij = xi::Json::object();
            ij.set("key", im.key);
            ij.set("size", (int64_t)im.jpeg.size());
            ij.set("b64", b64(im.jpeg));
            imgs.push(ij);
        }
        fj.set("images", imgs);
        xi::Json ext = xi::Json::array();
        for (const auto& kv : f.extra) ext.push(kv.first);
        fj.set("extra_keys", ext);
        fj.set("note", f.note);
        arr.push(fj);
    }

    for (const auto& f : frames_v3) {
        std::vector<uint8_t> bytes = encode_v3(f);
        std::string path = dir + "/" + f.name + ".bin";
        std::ofstream out(path, std::ios::binary);
        if (!out) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
        out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
        std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
        arr.push(manifest_v3(f));
    }
    manifest.set("frames", arr);

    std::string mpath = dir + "/manifest.json";
    std::ofstream mo(mpath, std::ios::binary);
    if (!mo) { std::fprintf(stderr, "cannot write %s\n", mpath.c_str()); return 1; }
    std::string text = manifest.dump_pretty();
    mo.write(text.data(), (std::streamsize)text.size());
    mo.put('\n');
    std::printf("wrote %s\n", mpath.c_str());
    return 0;
}

int verify(const std::string& dir, const std::vector<Frame>& frames,
           const std::vector<FrameV3>& frames_v3) {
    int failures = 0;
    for (const auto& f : frames) {
        std::vector<uint8_t> want;
        std::string path = dir + "/" + f.name + ".bin";
        if (!read_file(path, want)) {
            std::fprintf(stderr, "FAIL %s: missing golden (run --regen)\n", f.name.c_str());
            ++failures; continue;
        }
        std::vector<uint8_t> got = encode(f);
        if (got != want) {
            std::fprintf(stderr, "FAIL %s: encoder output (%zu B) != golden (%zu B)\n",
                         f.name.c_str(), got.size(), want.size());
            ++failures; continue;
        }
        std::printf("[xex1] ok %-12s %zu bytes\n", f.name.c_str(), got.size());
    }
    for (const auto& f : frames_v3) {
        std::vector<uint8_t> want;
        std::string path = dir + "/" + f.name + ".bin";
        if (!read_file(path, want)) {
            std::fprintf(stderr, "FAIL %s: missing golden (run --regen)\n", f.name.c_str());
            ++failures; continue;
        }
        std::vector<uint8_t> got = encode_v3(f);
        if (got != want) {
            std::fprintf(stderr, "FAIL %s: v3 encoder output (%zu B) != golden (%zu B)\n",
                         f.name.c_str(), got.size(), want.size());
            ++failures; continue;
        }
        std::printf("[xex1] ok %-12s %zu bytes (v3)\n", f.name.c_str(), got.size());
    }
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    const bool do_regen = (argc > 1 && std::string(argv[1]) == "--regen");
    const std::string dir = binary_dir();
    const std::vector<Frame> frames = build_frames();
    const std::vector<FrameV3> frames_v3 = build_frames_v3();

    if (do_regen) return regen(dir, frames, frames_v3);

    int failures = verify(dir, frames, frames_v3);
    if (failures == 0) {
        std::printf("\nALL XEX1 FIXTURE GOLDENS MATCH (%zu v1 + %zu v3 frames)\n",
                    frames.size(), frames_v3.size());
        return 0;
    }
    std::fprintf(stderr, "\n%d XEX1 FIXTURE MISMATCH(ES)\n", failures);
    return 1;
}
