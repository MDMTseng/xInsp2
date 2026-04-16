#pragma once
//
// xi_jpeg.hpp — encode xi::Image to JPEG bytes using stb_image_write.
//
// stb's JPEG writer accepts 1/3/4 channel interleaved uint8 buffers. For
// 4-channel input it drops alpha. Quality defaults to 85 — tuned for
// preview fidelity at reasonable size.
//
// The implementation lives in backend/src/stb_impl.cpp to avoid multiple
// definitions when this header is included from more than one TU.
//

#include <cstdint>
#include <vector>

#include "xi_image.hpp"

// Forward-declare the stb function we use. The full definition lives in
// backend/vendor/stb_image_write.h and is compiled into stb_impl.cpp.
extern "C" int stbi_write_jpg_to_func(
    void (*func)(void* context, void* data, int size),
    void* context,
    int x, int y, int comp, const void* data, int quality);

namespace xi {

inline bool encode_jpeg(const Image& img, int quality, std::vector<uint8_t>& out) {
    if (img.empty()) return false;
    if (img.channels != 1 && img.channels != 3 && img.channels != 4) return false;
    out.clear();
    auto writer = [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<uint8_t>*>(ctx);
        auto* p = static_cast<uint8_t*>(data);
        v->insert(v->end(), p, p + size);
    };
    int ok = stbi_write_jpg_to_func(writer, &out,
                                     img.width, img.height, img.channels,
                                     img.data(), quality);
    return ok != 0;
}

} // namespace xi
