// test_xex1_fixtures.cpp — generate + verify the XEX1 binary golden fixtures.
//
// The XEX1 frame is encoded by ONE C++ encoder (xex1_encode.hpp) and decoded by
// two independent hand-rolled msgpack readers — the expose plugin's webUI
// (plugins/expose/ui/index.html) and examples/lib/xex1.py. Nothing tested that
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

#include <xi/xi_json.hpp>

#include "xex1_encode.hpp"

using xi::xex1::EncImage;

namespace {

// Deterministic synthetic "JPEG" payload of a given size — content is irrelevant
// to the codec (images ride as opaque msgpack bin); the pattern just makes each
// image distinguishable so a decoder that mis-slices a bin blob is caught.
std::vector<uint8_t> synth(size_t n, uint8_t salt) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)((i * 37 + salt) & 0xFF);
    return v;
}

std::string b64(const std::vector<uint8_t>& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t* p = in.data(); size_t n = in.size();
    std::string o; o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t b = p[i] << 16;
        if (i + 1 < n) b |= p[i + 1] << 8;
        if (i + 2 < n) b |= p[i + 2];
        o.push_back(T[(b >> 18) & 63]); o.push_back(T[(b >> 12) & 63]);
        o.push_back(i + 1 < n ? T[(b >> 6) & 63] : '=');
        o.push_back(i + 2 < n ? T[b & 63] : '=');
    }
    return o;
}

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

// --- XEX1-v2 (the canonical frame dump) fixtures ----------------------------
//
// A v2 golden is a channel/seq + a generic entry list. Each field carries its
// value as a decoder-visible expectation AND is encoded into a V2Entry with its
// canonical msgpack bytes (scalars/str/bin via xi::mp::Writer, nested msgpack
// verbatim, image pixels inlined as bin) — the exact bytes the expose frame door
// emits for the same logical frame. The JS + Python decoders decode these same
// .bin files and assert the field values below.

struct V2Field {
    std::string          key;
    std::string          kind;   // "i64" | "f64" | "str" | "bin" | "mp" | "image"
    int64_t              i = 0;
    double               f = 0;
    std::string          s;      // str value
    std::vector<uint8_t> bytes;  // bin payload / mp canonical bytes / image pixels
    int32_t              w = 0, h = 0, c = 0;   // image dims
    xi::Json             mp_value = xi::Json();  // decoded expectation for an mp field
};

struct FrameV2 {
    std::string          name;
    std::string          channel;
    uint64_t             seq = 0;
    std::vector<V2Field> fields;
    std::string          note;
};

// Encode one FrameV2 into wire bytes through the SHARED v2 encoder.
std::vector<uint8_t> encode_v2(const FrameV2& f) {
    std::vector<xi::xex1::V2Entry> entries;
    entries.reserve(f.fields.size());
    for (const auto& fld : f.fields) {
        xi::xex1::V2Entry e;
        e.key = fld.key;
        if (fld.kind == "i64") {
            e.tag = XI_FRAME_TAG_I64; xi::mp::Writer w; w.int_(fld.i); e.value = w.take();
        } else if (fld.kind == "f64") {
            e.tag = XI_FRAME_TAG_F64; xi::mp::Writer w; w.float_(fld.f); e.value = w.take();
        } else if (fld.kind == "str") {
            e.tag = XI_FRAME_TAG_STR; xi::mp::Writer w; w.str(fld.s); e.value = w.take();
        } else if (fld.kind == "bin") {
            e.tag = XI_FRAME_TAG_BIN; xi::mp::Writer w; w.bin(fld.bytes.data(), fld.bytes.size());
            e.value = w.take();
        } else if (fld.kind == "mp") {
            e.tag = XI_FRAME_TAG_MP; e.value = fld.bytes;   // already-canonical nested bytes
        } else if (fld.kind == "image") {
            e.tag = XI_FRAME_TAG_IMAGE; e.w = fld.w; e.h = fld.h; e.c = fld.c;
            e.px = fld.bytes.data(); e.px_len = fld.bytes.size();
        }
        entries.push_back(std::move(e));
    }
    return xi::xex1::encode_frame_v2(f.channel, f.seq, entries);
}

std::vector<FrameV2> build_frames_v2() {
    std::vector<FrameV2> fs;

    fs.push_back({"v2_minimal", "c", 0, {}, "empty frame map — smallest v2 dump"});

    {
        FrameV2 f; f.name = "v2_scalars"; f.channel = "line1/cam0"; f.seq = 7;
        f.fields.push_back({"count", "i64", 42});
        f.fields.push_back({"neg", "i64", -5});
        f.fields.push_back({"big", "i64", 4294967296LL});      // 2^32 — still int64 0xd3
        {V2Field fl; fl.key = "score"; fl.kind = "f64"; fl.f = 1.5; f.fields.push_back(fl);}
        {V2Field fl; fl.key = "label"; fl.kind = "str"; fl.s = "ok"; f.fields.push_back(fl);}
        f.note = "scalar/str entries, canonical int64/float64/str32 verbatim on the wire";
        fs.push_back(std::move(f));
    }

    {
        FrameV2 f; f.name = "v2_image"; f.channel = "c"; f.seq = 3;
        V2Field img; img.key = "frame"; img.kind = "image";
        img.w = 2; img.h = 2; img.c = 1; img.bytes = {10, 20, 30, 40};   // raw pixels
        f.fields.push_back(std::move(img));
        f.note = "image descriptor {w,h,c,px} with raw pixels inlined as bin (doc 07 D2)";
        fs.push_back(std::move(f));
    }

    {
        // A nested msgpack entry (blob_analysis-shaped): "blobs" = [ {area, cx} ].
        // Built with the SAME xi::mp::Writer a producer uses; the decoders decode
        // it into a native list/array and compare against mp_value.
        FrameV2 f; f.name = "v2_nested"; f.channel = "c"; f.seq = 1;
        xi::mp::Writer w;
        w.array(2);
        w.map(2); w.key("area"); w.int_(100); w.key("cx"); w.float_(2.5);
        w.map(2); w.key("area"); w.int_(250); w.key("cx"); w.float_(8.0);
        V2Field blobs; blobs.key = "blobs"; blobs.kind = "mp"; blobs.bytes = w.take();
        auto arr = xi::Json::array();
        auto b0 = xi::Json::object(); b0.set("area", (int64_t)100).set("cx", 2.5); arr.push(b0);
        auto b1 = xi::Json::object(); b1.set("area", (int64_t)250).set("cx", 8.0); arr.push(b1);
        blobs.mp_value = arr;
        f.fields.push_back(std::move(blobs));
        f.note = "nested canonical msgpack (array of maps) rides verbatim — doc 07 D3";
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

// Manifest entry for one v2 frame: channel/seq + the decoded field expectations.
xi::Json manifest_v2(const FrameV2& f) {
    xi::Json fj = xi::Json::object();
    fj.set("file", f.name + ".bin");
    fj.set("v", 2);
    fj.set("channel", f.channel);
    fj.set("seq", (int64_t)f.seq);
    xi::Json fields = xi::Json::array();
    for (const auto& fld : f.fields) {
        xi::Json j = xi::Json::object();
        j.set("key", fld.key);
        j.set("kind", fld.kind);
        if (fld.kind == "i64")        j.set("value", fld.i);
        else if (fld.kind == "f64")   j.set("value", fld.f);
        else if (fld.kind == "str")   j.set("value", fld.s);
        else if (fld.kind == "bin") { j.set("b64", b64(fld.bytes)); j.set("size", (int64_t)fld.bytes.size()); }
        else if (fld.kind == "mp")    j.set("value", fld.mp_value);
        else if (fld.kind == "image") {
            j.set("w", fld.w); j.set("h", fld.h); j.set("c", fld.c);
            j.set("b64", b64(fld.bytes)); j.set("size", (int64_t)fld.bytes.size());
        }
        fields.push(j);
    }
    fj.set("fields", fields);
    fj.set("note", f.note);
    return fj;
}

int regen(const std::string& dir, const std::vector<Frame>& frames,
          const std::vector<FrameV2>& frames_v2) {
    xi::Json manifest = xi::Json::object();
    manifest.set("note",
        "XEX1 binary golden frames. Generated by plugins/expose/tests/test_xex1_fixtures.cpp "
        "--regen from the production encoder (plugins/expose/src/xex1_encode.hpp). Do not edit "
        "by hand. Each frame carries a `v` (1 = legacy display frame; 2 = canonical frame "
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

    for (const auto& f : frames_v2) {
        std::vector<uint8_t> bytes = encode_v2(f);
        std::string path = dir + "/" + f.name + ".bin";
        std::ofstream out(path, std::ios::binary);
        if (!out) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
        out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
        std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
        arr.push(manifest_v2(f));
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
           const std::vector<FrameV2>& frames_v2) {
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
    for (const auto& f : frames_v2) {
        std::vector<uint8_t> want;
        std::string path = dir + "/" + f.name + ".bin";
        if (!read_file(path, want)) {
            std::fprintf(stderr, "FAIL %s: missing golden (run --regen)\n", f.name.c_str());
            ++failures; continue;
        }
        std::vector<uint8_t> got = encode_v2(f);
        if (got != want) {
            std::fprintf(stderr, "FAIL %s: v2 encoder output (%zu B) != golden (%zu B)\n",
                         f.name.c_str(), got.size(), want.size());
            ++failures; continue;
        }
        std::printf("[xex1] ok %-12s %zu bytes (v2)\n", f.name.c_str(), got.size());
    }
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    const bool do_regen = (argc > 1 && std::string(argv[1]) == "--regen");
    const std::string dir = binary_dir();
    const std::vector<Frame> frames = build_frames();
    const std::vector<FrameV2> frames_v2 = build_frames_v2();

    if (do_regen) return regen(dir, frames, frames_v2);

    int failures = verify(dir, frames, frames_v2);
    if (failures == 0) {
        std::printf("\nALL XEX1 FIXTURE GOLDENS MATCH (%zu v1 + %zu v2 frames)\n",
                    frames.size(), frames_v2.size());
        return 0;
    }
    std::fprintf(stderr, "\n%d XEX1 FIXTURE MISMATCH(ES)\n", failures);
    return 1;
}
