//
// poly_door — the POLYMORPHIC DOOR teaching plugin: ONE xi.pack@1 door
// dispatching different behaviors on the pack's `type` entry, the same idiom
// the capability plane uses for `$v` (provider-internal dispatch; the host
// funnel forwards blind) — but on a PRODUCER-DOMAIN discriminator.
//
// The convention this example blesses (contract/canonical-profile-notes.md,
// "Producer-domain type dispatch"): the discriminator is a BARE `type` str
// entry, NOT `$type`. The `$` prefix is reserved for keys the FRAMEWORK
// reads/stamps ($fault family, $src/$prov, $seq; plane-owned $v/$probe/
// $versions); a which-of-MY-behaviors switch is producer schema and belongs
// in the plugin's declared keyset like any other input.
//
// Dispatch table (type_gate_ mirrors imgcodec's version_gate_):
//   type="measure"  -> stats over image "gray": pixels/nonzero/min/max/mean.
//   type="annotate" -> derived overlay entry: 0/255 mask of pixels > threshold
//                      (i64 "threshold", default 128) + marked count.
//   type missing    -> $fault missing_input      + "types" (supported set).
//   type unknown    -> $fault unsupported_type   + "types" (supported set) —
//                      the producer-domain mirror of the capability plane's
//                      unsupported-$v reply ($fault + $versions).
//
#include <xi/xi_abi.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {
constexpr const char* kTypes = "measure,annotate";   // the supported set
}

class PolyDoor : public xi::Plugin {
public:
    PolyDoor(const xi_host_api* host, const std::string& name)
        : xi::Plugin(host, name) {}

    using xi::Plugin::process;   // keep the Record overload visible (pack-only override)

    void process(xi::PackIn& in, xi::PackOut& out) override {
        // --- the type gate: dispatch on the producer-domain discriminator ---
        auto ty = in.str("type");
        if (!ty) {
            out.fault("missing_input", "type",
                      "poly_door: discriminator entry 'type' is missing")
               .str("types", kTypes);
            return;
        }
        if (*ty == "measure")  { measure_(in, out);  return; }
        if (*ty == "annotate") { annotate_(in, out); return; }
        out.fault("unsupported_type", "type",
                  "poly_door: unknown type '" + std::string(*ty) +
                      "'; supported: " + kTypes)
           .str("types", kTypes);
    }

private:
    // type="measure": whole-image stats, scalar results only.
    void measure_(xi::PackIn& in, xi::PackOut& out) {
        auto img = in.image("gray");
        if (!img || !img->pixels) {
            out.fault("missing_input", "gray",
                      "poly_door[measure]: required image entry 'gray' is missing");
            return;
        }
        const auto*  px = static_cast<const uint8_t*>(img->pixels);
        const size_t n  = (size_t)img->width * img->height * img->channels;
        uint8_t  mn = 255, mx = 0;
        uint64_t sum = 0, nz = 0;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t v = px[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += v;
            if (v) ++nz;
        }
        out.str("type", "measure")
           .i64("pixels", (int64_t)n)
           .i64("nonzero", (int64_t)nz)
           .i64("min", mn)
           .i64("max", mx)
           .f64("mean", n ? (double)sum / (double)n : 0.0);
    }

    // type="annotate": a DERIVED overlay entry — 0/255 mask of pixels > thr.
    void annotate_(xi::PackIn& in, xi::PackOut& out) {
        auto img = in.image("gray");
        if (!img || !img->pixels) {
            out.fault("missing_input", "gray",
                      "poly_door[annotate]: required image entry 'gray' is missing");
            return;
        }
        const int64_t thr = in.i64_or("threshold", 128);
        const auto*   px  = static_cast<const uint8_t*>(img->pixels);
        const size_t  n   = (size_t)img->width * img->height * img->channels;
        std::vector<uint8_t> overlay(n, 0);
        int64_t marked = 0;
        for (size_t i = 0; i < n; ++i)
            if ((int64_t)px[i] > thr) { overlay[i] = 255; ++marked; }
        out.str("type", "annotate")
           .i64("threshold_used", thr)
           .i64("marked", marked)
           .image("overlay", img->width, img->height, img->channels,
                  overlay.data());
    }
};

XI_PLUGIN_IMPL(PolyDoor)
XI_PLUGIN_PACK_DOOR(PolyDoor)
