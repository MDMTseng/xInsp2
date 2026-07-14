//
// invert.cpp — image-in, image-out.
//
// Demonstrates:
//   - reading an image entry from the input pack (by key) — in.image() rides
//     the frozen @1 xi/image blob adapter, so the read side is unchanged
//   - producing the output image as a self-describing xi/image BLOB (spec 30):
//     mint a headed buffer, write the inverted pixels straight into its
//     64B-aligned payload, adopt it zero-copy — no heap-to-pool memcpy
//   - the fail-loud contract: a missing required input is a normal sealed
//     pack stamped "$fault", never a silent default
//
// Script usage:
//   auto out = xi::use("invert0").process(pack);   // pack: image "src"
//   auto inverted = out.get_image("dst");
//

#include <xi/xi_abi.hpp>
#include <xi/xi_mp.hpp>   // canonical msgpack Writer — the xi/image blob descriptor

class Invert : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    void process(xi::PackIn& in, xi::PackOut& out) override {
        // in.image() returns nullopt when "src" is absent or not an image
        // entry. Its pixels are a zero-copy pool span shared with other
        // consumers — read them, never write through them.
        auto src = in.image("src");
        if (!src || !src->pixels) {
            out.fault("missing_input", "src", "invert: no 'src' image in input");
            return;
        }

        // Produce the output as a self-describing xi/image blob: mint the headed
        // buffer, invert straight into its 64B-aligned payload, adopt it — the
        // pack co-owns the pool buffer (addref), and we drop our mint ref. No
        // heap-to-pool memcpy across the ABI (spec 30's zero-copy producer path,
        // the successor to the old adopt_image pool-handle hand-off).
        const int w = src->width, h = src->height, c = src->channels;
        const int n = w * h * c;
        const uint8_t* sp = static_cast<const uint8_t*>(src->pixels);

        xi::mp::Writer dw;                        // {"t":"xi/image","w","h","c","dt"}
        dw.map(5);
        dw.key("t");  dw.str("xi/image");
        dw.key("w");  dw.int_(w);
        dw.key("h");  dw.int_(h);
        dw.key("c");  dw.int_(c);
        dw.key("dt"); dw.str("u8");
        void* pp = nullptr;
        xi_image_handle bh = out.blob_mint(dw.bytes().data(), (int32_t)dw.bytes().size(),
                                           (int64_t)n, &pp);
        if (!bh || !pp) {
            out.fault("no_blob_plane", "dst", "invert: host has no xi.pack@4 blob plane");
            return;
        }
        uint8_t* dp = static_cast<uint8_t*>(pp);
        for (int i = 0; i < n; ++i) dp[i] = (uint8_t)(255 - sp[i]);

        out.adopt_blob("dst", bh);
        host_->image_release(bh);   // pack holds its own addref now
        out.i64("pixels", n);
    }
};

XI_PLUGIN_IMPL(Invert)
// Publish the xi.pack@1 door (the sole data plane since the v12 ABI cut).
XI_PLUGIN_PACK_DOOR(Invert)
