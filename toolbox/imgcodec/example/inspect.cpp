// imgcodec example — the capability nobody calls on purpose.
//
// Read this script looking for the codec. It is not here. There is no
// xi::use("codec"), no encode call, no jpeg anywhere: `imgcodec` is a LIB
// plugin, so it has no data plane at all — nothing routes to it and it never
// emits. It sits beside the pipeline and REGISTERS two capabilities with the
// host (xi.jpeg.encode, xi.image.decode).
//
// The consumer is `expose`. On its way out to the UI it asks the host
// capability plane "is anyone providing xi.jpeg.encode?" and, if someone is,
// sends a full-resolution JPEG down the socket instead of the raw pixel plane.
// That is the entire point of a capability: the producer (this script) and the
// consumer (expose) never learn who — or whether — the provider is.
//
// So this file is just an inspection. It paints a fixed synthetic "part",
// measures the dark blob on it, and pushes the picture to the UI. Everything
// interesting happens underneath it:
//
//   * with `codec` present  -> the wire carries a JPEG at source resolution,
//     a fraction of the raw size, and the image is only EVER encoded ONCE:
//     these pixels are byte-identical every tick, and imgcodec keys its memo
//     cache on content, so tick 200 is still served by tick 0's encode.
//   * with `codec` deleted  -> expose flips to its raw path, prints
//     "jpeg preview OFF (raw fallback)", and keeps streaming. Nothing here
//     changes. Nothing fails. That is graceful degradation, and an optional
//     capability that cannot degrade gracefully is not optional.
//
// Try it live: delete the `codec` instance in the UI while this is running and
// watch the frames get fatter but not stop. driver.py does exactly that.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int W = 320, H = 240;

// The "part": a smooth gradient with a bright reference bar and one dark blob.
// FIXED content — identical bytes on every tick. That is deliberate: it makes
// the provider's content-keyed dedup observable from outside (its lifetime
// encode counter must stay at 1 no matter how long you leave this running).
const std::vector<uint8_t>& part() {
    static const std::vector<uint8_t> px = [] {
        std::vector<uint8_t> v((size_t)W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                v[(size_t)y * W + x] = (uint8_t)(60 + ((x + y) >> 2) % 120);
        for (int y = 20; y < 40; ++y)                    // reference bar
            for (int x = 20; x < 300; ++x) v[(size_t)y * W + x] = 245;
        for (int y = 120; y < 150; ++y)                  // the "defect"
            for (int x = 140; x < 190; ++x) v[(size_t)y * W + x] = 12;
        return v;
    }();
    return px;
}

}  // namespace

// The real tunable of this inspection. Nothing to do with the codec — that is
// the point; the script's vocabulary never mentions it.
xi::Param<int> dark_limit{"dark_limit", 40, xi::Range<int>{0, 255}};

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (t.is_active()) return;          // driven by the synthetic timer tick

    static long long seq = 0;
    const long long s = seq++;

    const auto& px = part();
    long long dark = 0;
    for (uint8_t v : px) if (v < (uint8_t)(int)dark_limit) ++dark;
    const double pct = 100.0 * (double)dark / (double)px.size();

    // Push the picture to the UI. Note what is NOT in this call: any statement
    // about compression. The script hands over pixels; whether they leave the
    // machine as JPEG or as a raw plane is a deployment property, decided by
    // which capability providers happen to be loaded.
    xi::ScriptPackBuilder b;
    b.add_str("$channel", "part");
    b.add_i64("$seq", s);
    b.add_image("img", W, H, 1, px.data());
    b.add_f64("dark_pct", pct);
    b.add_i64("dark_limit", dark_limit);
    auto out = b.seal();
    const bool pushed = out.valid() && xi::use("view").push(out);

    char msg[128];
    std::snprintf(msg, sizeof msg, "seq=%lld dark=%.2f%% (limit<%d)",
                  s, pct, (int)dark_limit);
    if (!pushed)      xi::ng(1, "could not push the frame to view");
    else if (pct < 5) xi::ok(1, msg);
    else              xi::ng(1, msg);
}
