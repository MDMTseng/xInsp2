// demo_pack_c_profile.cpp — a worked example of the design-C pack:
// a laser-profiler inspection frame carrying
//   pak["img_left"]  u8 tensor 1280x960x1   (camera 0)
//   pak["img_right"] u8 tensor 1280x960x1   (camera 1)
//   pak["profile"]   custom PROFILE_F32 blob: float32 height map 2048x400
//                    with a self-describing header (magic/version/dims/res)
//   pak["meta"]      canonical msgpack map (recipe, exposure_us, roi[4])
//   pak["serial"] / pak["station"]  raw scalars
// Build -> uniform pak["key"] reads -> serialize -> deserialize -> re-verify.
#include <xi/proto/xi_pack_c.hpp>
#include <xi/xi_mp.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace xi::packc;

// ---- the custom binary structure: a float32 3D scan profile --------------
// Self-describing header (offset-based, position-independent, POD) so the
// bytes survive record/replay and any consumer can verify before casting.
inline constexpr uint16_t kTypeProfileF32 = kTypeUserBase + 1;

struct ProfileF32Header {
    uint32_t magic;      // 'PRF1'
    uint16_t version;    // 1
    uint16_t _pad;
    uint32_t points;     // samples per profile (X)
    uint32_t lines;      // profiles per scan   (Y)
    float    x_res_um;   // lateral resolution
    float    z_res_um;   // height resolution
    // float data[points*lines] follows immediately
};
inline constexpr uint32_t kProfileMagic = 0x31465250; // 'PRF1'

int main() {
    // ---- produce -----------------------------------------------------------
    const uint32_t W = 1280, H = 960;
    std::vector<uint8_t> px(W * H);
    for (size_t i = 0; i < px.size(); ++i) px[i] = uint8_t(i * 7 & 0xFF);

    const uint32_t PTS = 2048, LINES = 400;
    std::vector<uint8_t> prof(sizeof(ProfileF32Header) + PTS * LINES * sizeof(float));
    auto* ph = reinterpret_cast<ProfileF32Header*>(prof.data());
    *ph = ProfileF32Header{kProfileMagic, 1, 0, PTS, LINES, 12.5f, 0.8f};
    auto* z = reinterpret_cast<float*>(prof.data() + sizeof(ProfileF32Header));
    for (uint32_t y = 0; y < LINES; ++y)
        for (uint32_t x = 0; x < PTS; ++x)
            z[y * PTS + x] = std::sin(x * 0.01f) * 50.f + y * 0.05f; // fake surface

    // metadata tree as canonical msgpack (encoded with the house codec)
    xi::mp::Writer mw;
    mw.map(3);
    mw.key("recipe");      mw.str("gasket-v7");
    mw.key("exposure_us"); mw.int_(1200);
    mw.key("roi");         mw.array(4);
    mw.int_(100); mw.int_(80); mw.int_(1180); mw.int_(880);

    PackBuilderC b;
    b.add_str("station", "L3-profiler");
    b.add_i64("serial", 20260713001LL);
    b.add_tensor("img_left",  W, H, 1, px.data());
    b.add_tensor("img_right", W, H, 1, px.data());
    b.add_blob("profile", kTypeProfileF32, prof.data(), prof.size());
    b.add_mp("meta", mw.bytes().data(), mw.bytes().size());
    PackHandleC pk = b.seal(/*ts_us=*/1730000000000000LL);

    // ---- consume: ONE namespace, uniform subscript --------------------------
    PackViewC pak(pk);
    std::printf("pack: %u entries, slab %llu bytes\n\n", pak.size(),
                (unsigned long long)pak.header()->slab_bytes);
    for (uint32_t i = 0; i < pak.size(); ++i) {
        auto e = pak.at(i);
        std::printf("  %-9s type=%3u storage=%s shape=%ux%ux%u\n",
                    std::string(e.key()).c_str(), e.type(),
                    e.storage() == kStorageExtern ? "EXTERN" : "INLINE",
                    e.shape(0), e.shape(1), e.shape(2));
    }

    std::printf("\nserial  = %lld\n", (long long)pak["serial"].as_i64());
    std::printf("station = %.*s\n", (int)pak["station"].as_str().size(),
                pak["station"].as_str().data());

    TensorViewC L = pak["img_left"].as_tensor();
    std::printf("img_left  %ux%ux%u, %llu B, 64B-aligned=%s, px[0]=%u\n",
                L.shape[0], L.shape[1], L.shape[2], (unsigned long long)L.bytes,
                (reinterpret_cast<uintptr_t>(L.data) % 64 == 0) ? "yes" : "no",
                L.data[0]);

    auto pb = pak["profile"].as_blob(kTypeProfileF32);   // type check here
    auto* rh = reinterpret_cast<const ProfileF32Header*>(pb.data());
    if (rh->magic != kProfileMagic || rh->version != 1) { std::puts("BAD PROFILE"); return 1; }
    auto* rz = reinterpret_cast<const float*>(pb.data() + sizeof(*rh));
    float zmin = 1e9f, zmax = -1e9f;
    for (uint32_t i = 0; i < rh->points * rh->lines; ++i) {
        zmin = rz[i] < zmin ? rz[i] : zmin;
        zmax = rz[i] > zmax ? rz[i] : zmax;
    }
    std::printf("profile   %ux%u @ %.1fum/%.2fum, z=[%.2f..%.2f] (zero-copy view)\n",
                rh->points, rh->lines, rh->x_res_um, rh->z_res_um, zmin, zmax);

    auto mp = pak["meta"].as_mp();
    std::printf("meta      msgpack %zu B (map32 tag=0x%02x)\n", mp.size(), mp[0]);

    // wrong-type read is fail-loud (Release: warn + null), not garbage:
    if (!pak["profile"].as_tensor().ok())
        std::puts("as_tensor() on the profile blob correctly refused (fail-loud)");

    // ---- serialize -> deserialize -> re-verify ------------------------------
    std::vector<uint8_t> wire = serialize(pk);
    PackHandleC pk2 = deserialize(wire.data(), wire.size());
    PackViewC pak2(pk2);
    auto pb2 = pak2["profile"].as_blob(kTypeProfileF32);
    bool same = pb2.size() == pb.size() && !std::memcmp(pb2.data(), pb.data(), pb.size());
    std::printf("\nwire %zu B -> round-trip profile bytes %s, serial=%lld\n",
                wire.size(), same ? "IDENTICAL" : "MISMATCH",
                (long long)pak2["serial"].as_i64());

    PackTableC::instance().release(pk);
    PackTableC::instance().release(pk2);
    std::puts(same ? "\nOK" : "\nFAIL");
    return same ? 0 : 1;
}
