// demo_pack_c_profile.cpp — a worked example of the design-C pack:
// a laser-profiler inspection frame carrying
//   pak["img_left"]  u8  tensor 1280x960x1  (camera 0)
//   pak["img_right"] u8  tensor 1280x960x1  (camera 1)
//   pak["profile"]   f32 tensor 2048x400x1  (height map — a FIRST-CLASS
//                    typed tensor, read back via as_tensor_of<float>())
//   pak["calib"]     custom CALIB_F32 blob: self-describing header
//                    (magic/version/res) — the escape hatch for structures
//                    the framework doesn't know
//   pak["meta"]      canonical msgpack map (recipe, exposure_us, roi[4])
//   pak["serial"] / pak["station"]  raw scalars
// Build -> uniform pak["key"] reads -> serialize -> deserialize -> re-verify.
// All lifetime is RAII (PackRef): zero manual retain/release in this file.
#include <xi/proto/xi_pack_c.hpp>
#include <xi/xi_mp.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace xi::packc;

// ---- the ONE custom binary structure: a calibration record ---------------
// Self-describing header (offset-based, position-independent, POD) so the
// bytes survive record/replay and any consumer can verify before casting.
inline constexpr uint16_t kTypeCalibF32 = kTypeUserBase + 1;

struct CalibF32Header {
    uint32_t magic;      // 'CAL1'
    uint16_t version;    // 1
    uint16_t _pad;
    float    x_res_um;   // lateral resolution
    float    z_res_um;   // height resolution
    uint32_t coeffs;     // polynomial coefficient count
    // float coeff[coeffs] follows immediately
};
inline constexpr uint32_t kCalibMagic = 0x314C4143; // 'CAL1'

int main() {
    // ---- produce -----------------------------------------------------------
    const uint32_t W = 1280, H = 960;
    std::vector<uint8_t> px(W * H);
    for (size_t i = 0; i < px.size(); ++i) px[i] = uint8_t(i * 7 & 0xFF);

    // The height map is a first-class f32 tensor now — no header, no cast.
    const uint32_t PTS = 2048, LINES = 400;
    std::vector<float> z(size_t(PTS) * LINES);
    for (uint32_t y = 0; y < LINES; ++y)
        for (uint32_t x = 0; x < PTS; ++x)
            z[y * PTS + x] = std::sin(x * 0.01f) * 50.f + y * 0.05f; // fake surface

    // The calibration record stays a typed BLOB: a structure only the
    // producer/consumer pair understands, guarded by its own magic/version.
    const uint32_t NCOEFF = 6;
    std::vector<uint8_t> cal(sizeof(CalibF32Header) + NCOEFF * sizeof(float));
    auto* ch = reinterpret_cast<CalibF32Header*>(cal.data());
    *ch = CalibF32Header{kCalibMagic, 1, 0, 12.5f, 0.8f, NCOEFF};
    auto* cc = reinterpret_cast<float*>(cal.data() + sizeof(CalibF32Header));
    for (uint32_t i = 0; i < NCOEFF; ++i) cc[i] = 1.0f / float(i + 1);

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
    b.add_tensor("img_left",  W, H, 1, px.data());              // u8 convenience
    b.add_tensor("img_right", W, H, 1, px.data());
    b.add_tensor("profile", PTS, LINES, 1, kDtypeF32, z.data()); // typed tensor
    b.add_blob("calib", kTypeCalibF32, cal.data(), cal.size());
    b.add_mp("meta", mw.bytes().data(), mw.bytes().size());
    PackRef pk = b.seal_ref(/*ts_us=*/1730000000000000LL);       // RAII owner

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
    std::printf("img_left  %ux%ux%u dtype=u8(%uB), %llu B, 64B-aligned=%s, px[0]=%u\n",
                L.shape[0], L.shape[1], L.shape[2], L.elem_size,
                (unsigned long long)L.bytes,
                (reinterpret_cast<uintptr_t>(L.data) % 64 == 0) ? "yes" : "no",
                L.data[0]);

    // element-typed read: dtype checked HERE, data comes back as const float*
    TensorOfC<float> P = pak["profile"].as_tensor_of<float>();
    float zmin = 1e9f, zmax = -1e9f;
    for (uint64_t i = 0; i < P.count; ++i) {
        zmin = P.data[i] < zmin ? P.data[i] : zmin;
        zmax = P.data[i] > zmax ? P.data[i] : zmax;
    }
    std::printf("profile   %ux%ux%u f32, %llu elems, z=[%.2f..%.2f] (zero-copy view)\n",
                P.shape[0], P.shape[1], P.shape[2],
                (unsigned long long)P.count, zmin, zmax);

    auto cb = pak["calib"].as_blob(kTypeCalibF32);   // type check here
    auto* rh = reinterpret_cast<const CalibF32Header*>(cb.data());
    if (rh->magic != kCalibMagic || rh->version != 1) { std::puts("BAD CALIB"); return 1; }
    std::printf("calib     %u coeffs @ %.1fum/%.2fum, coeff[2]=%.4f\n",
                rh->coeffs, rh->x_res_um, rh->z_res_um,
                reinterpret_cast<const float*>(cb.data() + sizeof(*rh))[2]);

    auto mp = pak["meta"].as_mp();
    std::printf("meta      msgpack %zu B (map tag=0x%02x)\n", mp.size(), mp[0]);

    // wrong-dtype read is fail-loud (Release: warn + null), not garbage:
    if (!pak["profile"].as_tensor_of<double>().ok())
        std::puts("as_tensor_of<double>() on the f32 profile correctly refused (fail-loud)");
    if (!pak["calib"].as_tensor().ok())
        std::puts("as_tensor() on the calib blob correctly refused (fail-loud)");

    // ---- serialize -> deserialize -> re-verify ------------------------------
    std::vector<uint8_t> wire = serialize(pk.get());
    PackRef pk2(deserialize(wire.data(), wire.size()));  // adopt the fresh rc=1
    PackViewC pak2(pk2);
    TensorOfC<float> P2 = pak2["profile"].as_tensor_of<float>();  // dtype survived
    bool same = P2.count == P.count &&
                !std::memcmp(P2.data, P.data, P.count * sizeof(float));
    std::printf("\nwire %zu B -> round-trip f32 profile bytes %s, serial=%lld\n",
                wire.size(), same ? "IDENTICAL" : "MISMATCH",
                (long long)pak2["serial"].as_i64());

    // No release calls: pk and pk2 are PackRefs — scope exit frees both packs.
    std::puts(same ? "\nOK" : "\nFAIL");
    return same ? 0 : 1;
}
