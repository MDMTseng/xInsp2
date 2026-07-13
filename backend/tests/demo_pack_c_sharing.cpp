// demo_pack_c_sharing.cpp — how images/binaries are SHARED on the design-C
// pack, zero-copy, across packs and beyond any pack's lifetime.
//
//   1. camera pattern : mint buffer -> write pixels IN PLACE -> adopt (0 copy)
//   2. stereo pattern : pack B adopts pack A's image (1 buffer, 2 packs)
//   3. producer dies  : pack A released; pack B still reads the same bytes
//   4. derived result : result pack references the original profile blob +
//                       adds its own metadata (no bytes moved)
//   5. bare retain    : a consumer addrefs the buffer itself; BOTH packs die;
//                       the buffer lives until the consumer's release
#include <xi/proto/xi_pack_c.hpp>

#include <cstdio>
#include <cstring>

using namespace xi::packc;

inline constexpr uint16_t kTypeProfileF32 = kTypeUserBase + 1;

static const char* aligned64(const void* p) {
    return (reinterpret_cast<uintptr_t>(p) % 64 == 0) ? "yes" : "no";
}

int main() {
    auto& bt = BufTable::instance();
    auto& pt = PackTableC::instance();

    // ---- 1. camera pattern: allocate first, write in place, adopt ---------
    const uint32_t W = 1920, H = 1200;
    const uint32_t shape[3] = {W, H, 1};
    BufHandleC cam = bt.mint(kTypeTensorU8, shape, uint64_t(W) * H, nullptr);
    uint8_t* dst = bt.writable_data(cam);          // camera/DMA writes HERE
    for (uint32_t i = 0; i < W * H; ++i) dst[i] = uint8_t(i & 0xFF);
    std::printf("1. minted frame buffer: %u B, 64B-aligned=%s (wrote in place, 0 copies)\n",
                W * H, aligned64(dst));

    PackBuilderC ba;
    ba.add_i64("serial", 1001);
    ba.adopt("img", cam);                           // pack A co-owns (rc 1->2)
    bt.release(cam);                                // hand over our ref (rc 2->1)
    // profile blob for scenario 4
    float zbuf[512];
    for (int i = 0; i < 512; ++i) zbuf[i] = i * 0.25f;
    ba.add_blob("profile", kTypeProfileF32, zbuf, sizeof zbuf);
    PackHandleC A = ba.seal();
    PackViewC pa(A);
    const uint8_t* pa_img = pa["img"].as_tensor().data;
    std::printf("   pack A img data = %p (same memory the camera wrote: %s)\n\n",
                (const void*)pa_img, pa_img == dst ? "YES" : "no");

    // ---- 2. stereo pattern: pack B adopts A's image ------------------------
    PackBuilderC bb;
    bb.add_i64("serial", 1002);
    bb.add_str("role", "stereo-mate");
    bb.adopt("img", pa["img"].buf_handle());        // zero-copy share
    PackHandleC B = bb.seal();
    PackViewC pb(B);
    std::printf("2. pack B adopted A's img: B data = %p -> %s pointer, no bytes copied\n\n",
                (const void*)pb["img"].as_tensor().data,
                pb["img"].as_tensor().data == pa_img ? "SAME" : "different");

    // ---- 4 (setup). derived-result pack referencing A's profile ------------
    PackBuilderC br;
    br.add_i64("serial", 1001);
    br.add_str("verdict", "PASS");
    br.add_f64("z_mean", 63.875);
    br.adopt("profile", pa["profile"].buf_handle()); // reference, not copy
    PackHandleC R = br.seal();
    PackViewC pr(R);
    std::printf("4. result pack references A's profile blob (%llu B) + its own verdict entries\n\n",
                (unsigned long long)bt.bytes(pr["profile"].buf_handle()));

    // ---- 5 (setup). a consumer retains the IMAGE BUFFER itself -------------
    BufHandleC kept = pb["img"].buf_handle();
    bt.addref(kept);                                 // bare buffer retain

    // ---- 3. producer dies first --------------------------------------------
    pt.release(A);                                   // pack A gone
    std::printf("3. pack A released. pack B img[0]=%u img[123]=%u  (still valid: %s)\n",
                pb["img"].as_tensor().data[0], pb["img"].as_tensor().data[123],
                pb["img"].as_tensor().data[123] == uint8_t(123) ? "YES" : "no");
    std::printf("   result pack profile[4]=%.2f (still valid: %s)\n\n",
                reinterpret_cast<const float*>(bt.data(pr["profile"].buf_handle()))[4],
                reinterpret_cast<const float*>(bt.data(pr["profile"].buf_handle()))[4] == 1.0f
                    ? "YES" : "no");

    // ---- 5. both packs die; the bare-retained buffer survives --------------
    pt.release(B);
    pt.release(R);
    const uint8_t* still = bt.data(kept);
    std::printf("5. ALL packs released. bare-retained buffer: data=%p [77]=%u (alive: %s)\n",
                (const void*)still, still ? still[77] : 0,
                (still && still[77] == 77) ? "YES" : "no");
    bt.release(kept);                                // last ref -> buffer returns to pool
    std::printf("   final release -> buffer returned to its size-class (stale read now null: %s)\n",
                bt.data(kept) == nullptr ? "YES" : "no");

    std::puts("\nOK");
    return 0;
}
