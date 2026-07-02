// xex1_encode.hpp — the XEX1 frame's msgpack encoder, as a standalone header.
//
// This is the SINGLE SOURCE OF TRUTH for how the `expose` plugin serializes a
// record to the wire. It was lifted out of expose.cpp so the same encoder can be
// driven by the golden-fixture generator/round-trip test
// (plugins/expose/tests/test_xex1_fixtures.cpp) — the frame bytes the plugin
// emits and the bytes the cross-implementation decoders (the plugin webUI's JS
// reader in ui/index.html, and examples/lib/xex1.py) are tested against are
// produced here and nowhere else.
//
// Only the msgpack subset the fixed XEX1 frame shape needs is implemented. Every
// width the encoder can emit has a boundary golden and a decoder cross-test:
//   uint : fixint / uint8 (0xCC) / uint16 (0xCD) / uint32 (0xCE) / uint64 (0xCF)
//   str  : fixstr / str8 (0xD9) / str16 (0xDA) / str32 (0xDB)
//   bin  : bin8 (0xC4) / bin16 (0xC5) / bin32 (0xC6)
//   arr  : fixarray / array16 (0xDC) / array32 (0xDD)
//   map  : fixmap / map16 (0xDE) / map32 (0xDF)
// Multi-byte integers are big-endian, per msgpack.
#ifndef XI_XEX1_ENCODE_HPP
#define XI_XEX1_ENCODE_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace xi {
namespace xex1 {

using Buf = std::vector<uint8_t>;

inline void mp_u16(Buf& f, uint16_t v) { f.push_back(v >> 8); f.push_back(v & 0xFF); }
inline void mp_u32(Buf& f, uint32_t v) { f.push_back(v >> 24); f.push_back((v >> 16) & 0xFF);
                                         f.push_back((v >> 8) & 0xFF); f.push_back(v & 0xFF); }
inline void mp_u64(Buf& f, uint64_t v) { for (int s = 56; s >= 0; s -= 8) f.push_back((v >> s) & 0xFF); }

// fixmap / map16 / map32. Was fixmap-only (silently wrapped n>15 into the low
// nibble and corrupted the frame); now widens like arrays/strings do. A frame
// whose top-level map grows past 15 keys (forward-compatible extension fields)
// is now encodable and round-trips through the widened decoders.
inline void mp_map(Buf& f, uint32_t n) {
    if (n <= 15) f.push_back(0x80 | (uint8_t)n);
    else if (n <= 0xFFFF) { f.push_back(0xDE); mp_u16(f, (uint16_t)n); }
    else { f.push_back(0xDF); mp_u32(f, n); }
}
inline void mp_arr(Buf& f, uint32_t n) {
    if (n <= 15) f.push_back(0x90 | (uint8_t)n);
    else if (n <= 0xFFFF) { f.push_back(0xDC); mp_u16(f, (uint16_t)n); }
    else { f.push_back(0xDD); mp_u32(f, n); }
}
inline void mp_uint(Buf& f, uint64_t v) {
    if (v <= 0x7F) f.push_back((uint8_t)v);
    else if (v <= 0xFF) { f.push_back(0xCC); f.push_back((uint8_t)v); }
    else if (v <= 0xFFFF) { f.push_back(0xCD); mp_u16(f, (uint16_t)v); }
    else if (v <= 0xFFFFFFFFull) { f.push_back(0xCE); mp_u32(f, (uint32_t)v); }
    else { f.push_back(0xCF); mp_u64(f, v); }
}
inline void mp_str(Buf& f, const std::string& s) {
    size_t n = s.size();
    if (n <= 31) f.push_back(0xA0 | (uint8_t)n);
    else if (n <= 0xFF) { f.push_back(0xD9); f.push_back((uint8_t)n); }
    else if (n <= 0xFFFF) { f.push_back(0xDA); mp_u16(f, (uint16_t)n); }
    else { f.push_back(0xDB); mp_u32(f, (uint32_t)n); }
    f.insert(f.end(), s.begin(), s.end());
}
inline void mp_bin(Buf& f, const uint8_t* p, size_t n) {
    if (n <= 0xFF) { f.push_back(0xC4); f.push_back((uint8_t)n); }
    else if (n <= 0xFFFF) { f.push_back(0xC5); mp_u16(f, (uint16_t)n); }
    else { f.push_back(0xC6); mp_u32(f, (uint32_t)n); }
    f.insert(f.end(), p, p + n);
}

// One already-JPEG-encoded image entry (key + compressed bytes).
struct EncImage {
    std::string          key;
    std::vector<uint8_t> jpeg;
};

// Build one atomic XEX1 frame: magic 'XEX1' + a msgpack map
//   { v:1, channel, seq, json, images:[ {key, jpeg} ] , <extra...> }.
// `extra` appends forward-compatible top-level uint fields after `images`; it is
// what pushes a frame's top-level map past the 15-key fixmap boundary in the
// golden fixtures (real production frames pass none). Decoders ignore unknown
// keys, so an extended frame stays a valid XEX1 frame.
inline std::vector<uint8_t> encode_frame(
        const std::string& channel, uint64_t seq, const std::string& values_json,
        const std::vector<EncImage>& images,
        const std::vector<std::pair<std::string, uint64_t>>& extra = {}) {
    Buf f;
    f.push_back('X'); f.push_back('E'); f.push_back('X'); f.push_back('1');
    mp_map(f, 5 + (uint32_t)extra.size());
    mp_str(f, "v");       mp_uint(f, 1);
    mp_str(f, "channel"); mp_str(f, channel);
    mp_str(f, "seq");     mp_uint(f, seq);
    mp_str(f, "json");    mp_str(f, values_json);
    mp_str(f, "images");
    mp_arr(f, (uint32_t)images.size());
    for (const auto& im : images) {
        mp_map(f, 2);
        mp_str(f, "key");  mp_str(f, im.key);
        mp_str(f, "jpeg"); mp_bin(f, im.jpeg.data(), im.jpeg.size());
    }
    for (const auto& kv : extra) {
        mp_str(f, kv.first); mp_uint(f, kv.second);
    }
    return f;
}

}  // namespace xex1
}  // namespace xi

#endif  // XI_XEX1_ENCODE_HPP
