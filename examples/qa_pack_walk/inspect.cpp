// qa_pack_walk — the three script-side pack READ surfaces beyond plain string
// gets, exercised PACK-ONLY in the live service (Gate P2 rows "generic
// producer-agnostic walk", "typed-schema reads", "cross-thread pack capture").
//
// mock_camera runs in PACK MODE (entries: seq:i64, frame:image 64x48x3). This
// script never names xi::Record; observability is the run_result verdict plane
// alone (pack-only end to end). Per pack it proves:
//
//   1. GENERIC WALK — for_each visits exactly the two entries with the right
//      ABI tags (XI_PACK_TAG_I64 / XI_PACK_TAG_IMAGE), the same producer-
//      agnostic enumeration expose/record_save do host-side, in script hands.
//   2. TYPED-SCHEMA READS — ScriptTypedPack over a declared keyset returns the
//      same values as the string-keyed reads (the _keys.h consumption pattern,
//      previously shown only in the manual examples/pack_pilot).
//   3. CROSS-THREAD CAPTURE — a ScriptPack copied BY VALUE into a worker thread
//      stays valid (its keepalive holds the pack + pool buffers), per the
//      lifetime contract in xi_use.hpp; worker and inspect-thread checksums of
//      the pixel buffer must agree.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>
#include <array>
#include <cstdio>
#include <future>
#include <string>
#include <string_view>

// The declared keyset of mock_camera's pack contract (contract/plugins/
// mock_camera.decl.json): slot order fixes key spelling at compile time.
struct CamPack {
    static constexpr std::array<std::string_view, 2> keys = { "seq", "frame" };
    enum : int { kSeq, kFrame };
};

static long long checksum(const xi::ScriptPackImage& img) {
    long long sum = 0;
    for (size_t i = 0; i < img.pixels.size(); i += 97) sum += img.pixels[i];
    return sum;
}

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;

    auto f = t.pack();
    if (!f) return;                                  // no pack on this tick → NA

    // 1. generic producer-agnostic walk.
    int n = 0;
    std::string keys;
    bool tags_ok = true;
    f.for_each([&](std::string_view k, int32_t tag) {
        ++n;
        if (!keys.empty()) keys += ',';
        keys.append(k);
        if (k == "seq"   && tag != XI_PACK_TAG_I64)   tags_ok = false;
        if (k == "frame" && tag != XI_PACK_TAG_IMAGE) tags_ok = false;
    });

    // 2. typed-schema reads agree with the string-keyed reads.
    auto typed = f.typed<CamPack>();
    int64_t seq_str   = f.get_i64("seq").value_or(-1);
    int64_t seq_typed = typed.get_i64<CamPack::kSeq>().value_or(-2);
    auto    img_typed = typed.get_image<CamPack::kFrame>();

    // 3. cross-thread capture: ScriptPack by value into a worker thread.
    auto fut = std::async(std::launch::async, [f]() -> long long {
        auto img = f.get_image("frame");             // read on the WORKER thread
        return img ? checksum(*img) : -1;
    });
    long long worker_sum = fut.get();
    long long local_sum  = -1;
    if (auto img = f.get_image("frame")) local_sum = checksum(*img);

    bool xthread_ok = worker_sum >= 0 && worker_sum == local_sum;
    bool pass = n == 2 && tags_ok && seq_str >= 0 && seq_str == seq_typed &&
                img_typed && img_typed->channels == 3 && xthread_ok;

    char msg[192];
    std::snprintf(msg, sizeof msg,
                  "walk n=%d keys=%s tags=%d seq=%lld typed=%lld xthread=%d",
                  n, keys.c_str(), tags_ok ? 1 : 0,
                  (long long)seq_str, (long long)seq_typed, xthread_ok ? 1 : 0);
    if (pass) xi::ok(1, msg);
    else      xi::ng(1, msg);
}
