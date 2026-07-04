//
// invert.cpp — image-in, image-out.
//
// Demonstrates:
//   - reading an image entry from the input pack (by key)
//   - allocating a fresh output image in the HOST POOL (pool_image) and
//     handing it to the output pack by refcount (adopt_image — zero-copy)
//   - the fail-loud contract: a missing required input is a normal sealed
//     pack stamped "$fault", never a silent default
//
// Script usage:
//   auto out = xi::use("invert0").process(pack);   // pack: image "src"
//   auto inverted = out.get_image("dst");
//

#include <xi/xi_abi.hpp>

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

        // A fresh output slot in the host's ImagePool: the invert loop writes
        // straight into pool memory, and adopt_image below hands the slot to
        // the pack by refcount — no heap-to-pool memcpy across the ABI.
        xi::Image dst = pool_image(src->width, src->height, src->channels);
        const uint8_t* sp = static_cast<const uint8_t*>(src->pixels);
        uint8_t*       dp = dst.write();   // blessed writable-output accessor
        const int n = src->width * src->height * src->channels;
        for (int i = 0; i < n; ++i) dp[i] = (uint8_t)(255 - sp[i]);

        out.adopt_image("dst", dst.width, dst.height, dst.channels, dst.pool_handle());
        out.i64("pixels", n);
    }
};

XI_PLUGIN_IMPL(Invert)
// Publish the xi.pack@1 door (the sole data plane since the v12 ABI cut).
XI_PLUGIN_PACK_DOOR(Invert)
