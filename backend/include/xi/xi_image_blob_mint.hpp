#pragma once
//
// xi_image_blob_mint.hpp — the PRODUCER-side (write) helpers for the `xi/image`
// convention type carried in a self-describing Pack blob (spec 30,
// docs/new_gen/30-self-describing-blob-plane.md). The write mirror of the read
// side in <xi/xi_image_blob.hpp>.
//
// These are CONVENTION-LAYER sugar, NOT core: the core container (xi_pack.hpp)
// owns the type-agnostic blob mechanism (BlobDesc / mint_blob / adopt_blob /
// get_blob / type_of) and knows no domain type. The `xi/image` type string and
// the "dt" element-size table live HERE, on top of that mechanism — the single
// dtype table is `image_blob_dt_elem_size` (xi_image_blob.hpp), reused so the
// convention has exactly one source of truth (doc 28 finding A②).
//
#include "xi_pack.hpp"      // xi::BlobDesc / xi::mint_blob / xi::BufRef / mp::Bytes
#include "xi_image_dt.hpp"  // xi::image_blob_dt_elem_size — the ONE dtype table
                            // (the minimal table header, NOT the full read side —
                            // keeps the reader's xi_mp_build.hpp out of the door's
                            // include graph, so a producer's local skip_value/
                            // as_str helpers don't collide via ADL)

#include <cstdint>
#include <string_view>

namespace xi {

// Build an `xi/image` descriptor: {"t":"xi/image","w","h","c","dt"}. The read
// side (read_image_blob) and the @1 get_image adapter consume this exact shape.
// Returns empty on an unknown dtype (the ONE table decides what "dt" means).
inline mp::Bytes make_image_desc(int32_t w, int32_t h, int32_t c,
                                 std::string_view dt = "u8") {
    if (image_blob_dt_elem_size(dt) == 0) return {};
    return BlobDesc("xi/image")
        .i64("w", w).i64("h", h).i64("c", c).str("dt", dt)
        .build();
}

// Mint an `xi/image` blob: build the descriptor + mint_blob with payload sized
// w*h*c*elem_size(dt). The convenience the SDK image producer sits on. Empty
// BufRef on a non-positive dim or an unknown dtype.
inline BufRef mint_image(int32_t w, int32_t h, int32_t c, std::string_view dt = "u8") {
    if (w <= 0 || h <= 0 || c <= 0) return {};
    const int64_t elem = image_blob_dt_elem_size(dt);
    if (elem == 0) return {};
    mp::Bytes desc = make_image_desc(w, h, c, dt);
    if (desc.empty()) return {};
    return mint_blob(desc.data(), int32_t(desc.size()),
                     int64_t(w) * h * c * elem);
}

}  // namespace xi
