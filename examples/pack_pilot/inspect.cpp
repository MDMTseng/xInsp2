// pack_pilot — the wave-2 Pack-plane pilot, driven END TO END from a script.
//
// EXPERIMENTAL (docs/new_gen/08 Wave 2, step 4): this exercises the minimal
// t.pack() script surface. mock_camera runs in PACK MODE (config
// pack_mode=true), so instead of a Record it emits a v3 Pack on the xi.pack@1
// data plane. That pack rides the dual-carry dispatch path to this script, which
// reads it back through t.pack() — a borrowed, opaque-handle-backed view (a
// script is its own JIT DLL and cannot touch the host Pack container's C++
// layout, so it reads by key string through the host's pack vtable).
//
// Per emitted pack the script:
//   * reads "seq" (the frame counter) and the "frame" image descriptor;
//   * publishes a per-pack verdict with xi::ok / xi::ng (the ONE run_result);
//   * surfaces seq + dims on the `expose` channel "qa" so a driver can watch the
//     sequence advance (VAR/EMIT are no-ops in this core; expose is the data-out).
//
// v0 SCOPE (the honest smallest surface): the script VERDICTS ON THE PACK'S OWN
// FIELDS. It does NOT chain the pack into blob_analysis's pack door — driving a
// plugin's pack door from a script would need new use()-pack plumbing (a host
// thunk + dispatch wiring), which is more than a thin shim; that chaining stays
// host-mock-tested in plugins/pack_pilot_test.cpp. See write-a-script.md.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>

#include <array>
#include <string_view>

// A tiny declared keyset for the TYPED view. Any struct with a constexpr `keys`
// array works (an xi::PackSchema-derived type, or the wave-3 generated _keys);
// here we spell mock_camera's pack contract inline to keep the example
// self-contained. get_i64<kSeq>() reads by this key string through the door — the
// schema just fixes the spelling at compile time (a bad slot is a compile error).
struct CamPack {
    static constexpr std::array<std::string_view, 2> keys = { "seq", "frame" };
    enum { kSeq, kFrame };
};

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;             // skip synthetic timer ticks

    auto f = t.pack();
    if (!f) {
        // No pack on this event (Record-era path, or pack plane absent). The
        // pilot's whole point is the pack path, so a packless tick is NA, not NG.
        return;
    }

    // Read through the schema-typed view (compile-time-checked key spelling).
    auto tf = f.typed<CamPack>();
    int64_t seq = tf.get_i64<CamPack::kSeq>().value_or(-1);
    auto    img = tf.get_image<CamPack::kFrame>();
    int w = img ? img->width    : 0;
    int h = img ? img->height   : 0;
    int c = img ? img->channels : 0;

    // Verdict: a well-formed camera frame is a 3-channel image with a real seq.
    bool ok = seq >= 0 && img && c == 3 && w > 0 && h > 0;
    if (ok) xi::ok(1, "frame ok");
    else    xi::ng(1, "malformed frame");

    // Surface the per-pack facts on channel "qa" for the driver to observe.
    xi::Record rec;
    rec.set("seq", (int)seq).set("w", w).set("h", h).set("c", c);
    rec.set("$channel", "qa");
    xi::use("expose").process(rec);
}
