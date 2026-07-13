// demo_pack_c_sharing.cpp — how images/binaries are SHARED on the design-C
// pack, zero-copy, across packs and beyond any pack's lifetime — RAII style:
// every lifetime below is a PackRef/BufRef scope; there is not one manual
// retain/release in this file, and the scope braces ARE the destruction
// points.
//
//   1. camera pattern : mint into a BufRef -> write pixels IN PLACE -> adopt
//                       (0 copies); the BufRef's scope-exit hands sole
//                       ownership to the pack
//   2. stereo pattern : pack B adopts pack A's image (1 buffer, 2 packs)
//   3. producer dies  : PackRef A leaves scope; pack B still reads the bytes
//   4. derived result : result pack references the original profile blob +
//                       adds its own metadata (no bytes moved)
//   5. kept BufRef    : a consumer takes `BufRef kept = pb["img"].buf_ref();`
//                       BOTH packs die at scope exit; the buffer lives until
//                       kept's release
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

    BufRef kept;                       // scenario 5: outlives every pack below
    const uint8_t* cam_dst = nullptr;  // where the "camera" wrote (ptr compare)

    { // ======= packs B and R live exactly this long =======================
        PackRef B, R;

        { // ===== pack A (the producer) lives exactly this long ============
            // ---- 1. camera pattern: mint, write in place, adopt ---------
            const uint32_t W = 1920, H = 1200;
            PackBuilderC ba;
            ba.add_i64("serial", 1001);
            { // the camera's own ref lives exactly this long
                const uint32_t shape[3] = {W, H, 1};
                BufRef cam(bt.mint(kTypeTensorU8, shape, uint64_t(W) * H,
                                   nullptr));            // adopts the mint's rc=1
                uint8_t* dst = bt.writable_data(cam.get()); // camera/DMA writes HERE
                for (uint32_t i = 0; i < W * H; ++i) dst[i] = uint8_t(i & 0xFF);
                cam_dst = dst;
                std::printf("1. minted frame buffer: %u B, 64B-aligned=%s (wrote in place, 0 copies)\n",
                            W * H, aligned64(dst));
                ba.adopt("img", cam);   // pack takes its OWN ref; cam untouched
            }                           // <- cam dies: the pack is now sole owner
            // profile blob for scenario 4
            float zbuf[512];
            for (int i = 0; i < 512; ++i) zbuf[i] = i * 0.25f;
            ba.add_blob("profile", kTypeProfileF32, zbuf, sizeof zbuf);
            PackRef A = ba.seal_ref();
            PackViewC pa(A);
            const uint8_t* pa_img = pa["img"].as_tensor().data;
            std::printf("   pack A img data = %p (same memory the camera wrote: %s)\n\n",
                        (const void*)pa_img, pa_img == cam_dst ? "YES" : "no");

            // ---- 2. stereo pattern: pack B adopts A's image --------------
            PackBuilderC bb;
            bb.add_i64("serial", 1002);
            bb.add_str("role", "stereo-mate");
            bb.adopt("img", pa["img"].buf_ref());       // zero-copy share
            B = bb.seal_ref();
            PackViewC pb(B);
            std::printf("2. pack B adopted A's img: B data = %p -> %s pointer, no bytes copied\n\n",
                        (const void*)pb["img"].as_tensor().data,
                        pb["img"].as_tensor().data == pa_img ? "SAME" : "different");

            // ---- 4 (setup). derived-result pack referencing A's profile --
            PackBuilderC br;
            br.add_i64("serial", 1001);
            br.add_str("verdict", "PASS");
            br.add_f64("z_mean", 63.875);
            br.adopt("profile", pa["profile"].buf_ref()); // reference, not copy
            R = br.seal_ref();
            PackViewC pr(R);
            std::printf("4. result pack references A's profile blob (%llu B) + its own verdict entries\n\n",
                        (unsigned long long)pr["profile"].as_blob(kTypeProfileF32).size());

            // ---- 5 (setup). a consumer keeps a BufRef on the image itself -
            kept = pb["img"].buf_ref();                  // owning co-ref
        } // <- ---- 3. producer dies FIRST: PackRef A destroyed HERE --------

        PackViewC pb(B), pr(R);
        std::printf("3. pack A out of scope. pack B img[0]=%u img[123]=%u  (still valid: %s)\n",
                    pb["img"].as_tensor().data[0], pb["img"].as_tensor().data[123],
                    pb["img"].as_tensor().data[123] == uint8_t(123) ? "YES" : "no");
        auto prof = pr["profile"].as_blob(kTypeProfileF32);
        float p4 = reinterpret_cast<const float*>(prof.data())[4];
        std::printf("   result pack profile[4]=%.2f (still valid: %s)\n\n",
                    p4, p4 == 1.0f ? "YES" : "no");
    } // <- ---- 5. BOTH packs die HERE (PackRefs B and R destroyed) ---------

    const uint8_t* still = bt.data(kept.get());
    std::printf("5. ALL packs out of scope. kept BufRef: data=%p [77]=%u (alive: %s)\n",
                (const void*)still, still ? still[77] : 0,
                (still && still[77] == 77) ? "YES" : "no");
    BufHandleC raw = kept.get();       // peek the wire handle before letting go
    kept.reset();                      // RAII release: last ref -> back to pool
    std::printf("   kept.reset() -> buffer returned to its size-class (stale read now null: %s)\n",
                bt.data(raw) == nullptr ? "YES" : "no");

    std::puts("\nOK");
    return 0;
}
