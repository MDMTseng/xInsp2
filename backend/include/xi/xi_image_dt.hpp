#pragma once
//
// xi_image_dt.hpp — the ONE `xi/image` "dt" element-size table (spec 30,
// doc 28 finding A②). Extracted into a dependency-free header so BOTH the read
// side (xi_image_blob.hpp) and the write side (xi_image_blob_mint.hpp) share a
// single source of truth WITHOUT the write side dragging the reader's msgpack
// machinery (xi_mp_build.hpp) into every TU that mints an image blob.
//
// This is CONVENTION-LAYER data, not core: the core container knows no dtype
// ladder. Depends on nothing but the standard library.
//
#include <cstdint>
#include <string_view>

namespace xi {

// Element byte size for the `xi/image` "dt" convention strings; 0 = unknown.
inline int64_t image_blob_dt_elem_size(std::string_view dt) {
    if (dt == "u8")  return 1;
    if (dt == "u16") return 2;
    if (dt == "i32") return 4;
    if (dt == "f32") return 4;
    if (dt == "f64") return 8;
    return 0;
}

}  // namespace xi
