// test_canonical_xcheck.cpp — the C++ leg of the THREE-WAY cross-validation.
//
// The canonical profile is implemented independently in C++ (xi/xi_mp.hpp),
// Python (tools/xinsp2_py/xinsp2/canonical.py), and TypeScript
// (ui-components/src/canonical-mp.mjs). Three artefacts hold them in lockstep:
//   * the C++ golden fixtures (protocol/fixtures/canonical/*.bin) — Python & Node
//     decode+recanonicalize them and must reproduce the same bytes (their legs);
//   * the shared portable vectors (contract/canonical-vectors.json) — Python &
//     Node already check them; THIS test adds the C++ leg: it builds each
//     vector's `value` from the portable node description, encodes with the C++
//     Writer, and byte-compares to the expected hex, then recanonicalizes each
//     recanon vector and compares to its expected output.
//
// Byte agreement across all three legs is the cross-language validation of the
// profile. A mismatch is a SPEC bug (the BINDING rulings in
// contract/canonical-profile-notes.md decide who is right), not a codec bug to
// paper over.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <xi/xi_json.hpp>
#include <xi/xi_mp.hpp>

using xi::mp::Bytes;
using xi::mp::Reader;
using xi::mp::Status;
using xi::mp::Writer;

namespace {

int g_fail = 0;
#define XC(cond, msg) do { if (!(cond)) { \
    std::printf("  FAIL: %s\n", msg); ++g_fail; } } while (0)

std::string to_hex(const Bytes& b) {
    static const char* H = "0123456789abcdef";
    std::string o;
    o.reserve(b.size() * 2);
    for (uint8_t c : b) { o.push_back(H[c >> 4]); o.push_back(H[c & 0xf]); }
    return o;
}

Bytes from_hex(const std::string& h) {
    Bytes b;
    b.reserve(h.size() / 2);
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        b.push_back((uint8_t)((nyb(h[i]) << 4) | nyb(h[i + 1])));
    return b;
}

// Build a portable node (the same schema the Python `_build` / JS `build` use)
// into the canonical Writer. This is the C++ mirror of those sibling builders.
void build_node(const xi::Json& node, Writer& w) {
    std::string t = node["t"].as_string();
    if (t == "null") {
        w.nil();
    } else if (t == "bool") {
        w.boolean(node["v"].as_bool());
    } else if (t == "int") {
        const xi::Json v = node["v"];
        if (v.is_string()) {
            // Big values arrive as decimal strings (int64 boundaries, uint64).
            std::string s = v.as_string();
            if (!s.empty() && s[0] == '-') {
                w.int_((int64_t)std::strtoll(s.c_str(), nullptr, 10));
            } else {
                // uint_ emits int64 for <= INT64_MAX and uint64 above — the profile.
                w.uint_((uint64_t)std::strtoull(s.c_str(), nullptr, 10));
            }
        } else {
            // Numeric ints in the vectors all fit exactly in a double, so the
            // round-trip through as_double() is lossless.
            w.int_((int64_t)v.as_double());
        }
    } else if (t == "f64") {
        const xi::Json bits = node["bits"];
        if (bits.is_string()) {
            uint64_t raw = std::strtoull(bits.as_string().c_str(), nullptr, 16);
            double d;
            std::memcpy(&d, &raw, sizeof d);
            w.float_(d);
        } else {
            w.float_(node["v"].as_double());
        }
    } else if (t == "str") {
        w.str(node["v"].as_string());
    } else if (t == "bin") {
        Bytes raw = from_hex(node["hex"].as_string());
        w.bin(raw.data(), raw.size());
    } else if (t == "array") {
        const xi::Json arr = node["v"];
        w.array((uint32_t)arr.size());
        arr.for_each([&](const char*, const xi::Json& child) { build_node(child, w); });
    } else if (t == "map") {
        const xi::Json arr = node["v"];   // array of [key, valueNode] pairs
        w.map((uint32_t)arr.size());
        arr.for_each([&](const char*, const xi::Json& pair) {
            w.key(pair[0].as_string());
            build_node(pair[1], w);
        });
    } else {
        std::printf("  FAIL: unknown node type '%s'\n", t.c_str());
        ++g_fail;
    }
}

std::string vectors_path() {
    if (const char* p = std::getenv("XINSP2_VECTORS")) return p;
    return "../../contract/canonical-vectors.json";
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace

int main() {
    std::printf("test_canonical_xcheck (C++ leg vs contract/canonical-vectors.json)\n");
    std::string text;
    const std::string path = vectors_path();
    if (!read_file(path, text)) {
        std::fprintf(stderr, "cannot read %s (set XINSP2_VECTORS)\n", path.c_str());
        return 2;
    }
    xi::Json doc = xi::Json::parse(text);

    int n_encode = 0, n_recanon = 0;

    // ENCODE leg: build the value, encode, byte-compare to expected hex.
    doc["encode"].for_each([&](const char*, const xi::Json& c) {
        std::string name = c["name"].as_string();
        Writer w;
        build_node(c["value"], w);
        std::string got = to_hex(w.bytes());
        std::string want = c["hex"].as_string();
        if (got != want) {
            std::printf("  FAIL encode %-24s\n    got  %s\n    want %s\n",
                        name.c_str(), got.c_str(), want.c_str());
            ++g_fail;
        }
        ++n_encode;
    });

    // RECANON leg: recanonicalize the compact input, byte-compare to expected.
    doc["recanon"].for_each([&](const char*, const xi::Json& c) {
        std::string name = c["name"].as_string();
        Bytes in = from_hex(c["in_hex"].as_string());
        Writer out;
        Status s = Reader(in).canonicalize(out);
        if (s != Status::Ok) {
            std::printf("  FAIL recanon %-24s canonicalize -> %s\n",
                        name.c_str(), xi::mp::status_str(s));
            ++g_fail;
        } else {
            std::string got = to_hex(out.bytes());
            std::string want = c["out_hex"].as_string();
            if (got != want) {
                std::printf("  FAIL recanon %-24s\n    got  %s\n    want %s\n",
                            name.c_str(), got.c_str(), want.c_str());
                ++g_fail;
            }
        }
        ++n_recanon;
    });

    if (g_fail == 0) {
        std::printf("  OK: %d encode + %d recanon vectors byte-match the C++ codec\n",
                    n_encode, n_recanon);
        return 0;
    }
    std::fprintf(stderr, "  %d cross-check MISMATCH(ES)\n", g_fail);
    return 1;
}
