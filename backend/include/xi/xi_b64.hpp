#pragma once
//
// xi_b64.hpp — the ONE base64 encoder (RFC 4648, standard alphabet, '=' pad).
//
// Three headers/TUs used to carry byte-identical copies of this loop
// (xi_ws_server's handshake `base64`, expose's frame_b64 `b64`, the XEX1
// fixture tool's manifest `b64`), each private to its file. This header IS
// the minimal shared leaf: std-lib only, no platform/protocol coupling — so
// every layer can route through it and stay a leaf. Encode only; nothing in
// the tree decodes base64 in C++ (the decoders live in JS/Python clients).
//
#include <cstdint>
#include <string>

namespace xi {

inline std::string b64_encode(const void* data, size_t n) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t* in = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= (uint32_t)in[i + 2];
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(i + 1 < n ? tbl[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < n ? tbl[v & 63]        : '=');
    }
    return out;
}

} // namespace xi
