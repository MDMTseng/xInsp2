//
// invert.cpp — image-in, image-out.
//
// Demonstrates:
//   - reading an image from the input Record (by key)
//   - allocating a new image of the same size/channels
//   - writing an image into the output Record
//
// Script usage:
//   auto& inv = xi::use("invert0");
//   auto out = inv.process(xi::Record().image("src", my_img));
//   auto inverted = out.get_image("dst");
//

#include <xi/xi_abi.hpp>

class Invert : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        const xi::Image& src = input.get_image("src");
        if (src.empty()) return xi::Record().set("error", "no 'src' image in input");

        xi::Image dst(src.width, src.height, src.channels);
        const uint8_t* sp = src.data();
        uint8_t*       dp = dst.data();
        const int n = src.width * src.height * src.channels;
        for (int i = 0; i < n; ++i) dp[i] = (uint8_t)(255 - sp[i]);

        return xi::Record().image("dst", dst).set("pixels", n);
    }
};

XI_PLUGIN_IMPL(Invert)
