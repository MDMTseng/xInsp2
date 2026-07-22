// qa_cap_imgcodec — the capability plane pilot LIVE (docs/new_gen/14):
// script-built image pack -> consumer plugin's pack door -> host-forwarded
// "xi.jpeg.encode" capability (provided by the imgcodec LIB instance, which
// has NO data plane) -> JPEG bytes + dedup counters back to the script, plus
// the "xi.image.decode" round trip inside the consumer.
//
// Per tick (synthetic timer drives — no camera needed):
//   1. Build a FIXED 16x16 gray pack (5x5 white square) with
//      xi::ScriptPackBuilder — identical content every run, so the provider's
//      content-keyed memo cache must dedup across calls AND across runs.
//   2. Call xi::use("enc").process(pack) TWICE — two independent consumer
//      calls for the same sealed content. Assert: both funnel rcs 0, JPEG SOI
//      magic bytes, the two JPEGs byte-identical, the SECOND call is a cache
//      hit, and the provider's lifetime encode counter stays at 1 (ONE encode
//      serves every consumer, forever) — the doc-14 motivating case.
//   3. The consumer also round-trips the bytes through "xi.image.decode";
//      dec_ok certifies the decoded dims match the source.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <algorithm>
#include <cstdio>
#include <vector>

XI_INSPECT_ENTRY(t, frame) {
    (void)frame; (void)t;
    static long long run = 0;
    const long long seq = run++;

    // ---- 1. the FIXED synthetic image (identical bytes every run) ----------
    constexpr int W = 16, H = 16;
    std::vector<uint8_t> gray((size_t)W * H, 0);
    for (int y = 5; y < 10; ++y)
        for (int x = 5; x < 10; ++x) gray[(size_t)y * W + x] = 255;

    xi::ScriptPackBuilder b;
    bool built = b.valid();
    built = b.add_image("gray", W, H, 1, gray.data()) && built;
    built = b.add_i64("quality", 90) && built;
    auto in = b.seal();
    built = built && in.valid();

    // ---- 2. two consumer calls, same sealed content -------------------------
    auto r1 = xi::use("enc").process(in);
    auto r2 = xi::use("enc").process(in);

    const long long rc1 = r1.get_i64("enc_rc").value_or(-99);
    const long long rc2 = r2.get_i64("enc_rc").value_or(-99);
    auto j1 = r1.get_bin("jpeg");
    auto j2 = r2.get_bin("jpeg");
    const bool magic = j1 && j1->size() > 3 &&
                       (*j1)[0] == 0xFF && (*j1)[1] == 0xD8 && (*j1)[2] == 0xFF;
    const bool same = j1 && j2 && j1->size() == j2->size() &&
                      std::equal(j1->begin(), j1->end(), j2->begin());
    const long long hit2    = r2.get_i64("cache_hit").value_or(-1);
    const long long encodes = r2.get_i64("encodes").value_or(-1);
    const long long dec1    = r1.get_i64("dec_ok").value_or(0);
    const long long dec2    = r2.get_i64("dec_ok").value_or(0);
    const bool fault = r1.get_str("$fault").has_value() ||
                       r2.get_str("$fault").has_value();

    // encodes must stay pinned at 1 FOREVER (identical content + quality):
    // one encode served every call of every run — the dedup headline.
    const bool pass = built && !fault && rc1 == 0 && rc2 == 0 && magic &&
                      same && hit2 == 1 && encodes == 1 && dec1 == 1 && dec2 == 1;

    char msg[192];
    std::snprintf(msg, sizeof msg,
                  "capqa seq=%lld built=%d fault=%d rc1=%lld rc2=%lld magic=%d "
                  "same=%d hit2=%lld enc=%lld dec1=%lld dec2=%lld",
                  seq, built ? 1 : 0, fault ? 1 : 0, rc1, rc2, magic ? 1 : 0,
                  same ? 1 : 0, hit2, encodes, dec1, dec2);
    if (pass) xi::ok(1, msg);
    else      xi::ng(1, msg);
}
