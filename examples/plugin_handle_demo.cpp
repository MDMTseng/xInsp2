//
// plugin_handle_demo.cpp — call blob_analysis plugin from a user script
// via PluginHandle. No need to include the plugin's source code.
//

#include <xi/xi.hpp>
#include <xi/xi_image.hpp>
#include <xi/xi_record.hpp>
#include <xi/xi_plugin_handle.hpp>

// Tunable params
xi::Param<int> thresh{"threshold", 100, {0, 255}};
xi::Param<int> min_area{"min_area", 30, {1, 10000}};

// Plugin handle — loads blob_analysis.dll at runtime via C ABI
xi::PluginHandle blobs{"detector0", "blob_analysis"};

// Generate a test image with bright spots on dark background
static xi::Image make_test_image(int frame) {
    int w = 320, h = 240;
    xi::Image img(w, h, 3);
    uint8_t* p = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int i = (y * w + x) * 3;
            p[i+0] = p[i+1] = p[i+2] = (uint8_t)(25 + x * 15 / w);
        }
    // Bright circles that move with frame
    auto circle = [&](int cx, int cy, int r, uint8_t v) {
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx)
                if (dx*dx + dy*dy <= r*r) {
                    int px = cx+dx, py = cy+dy;
                    if (px >= 0 && px < w && py >= 0 && py < h) {
                        int i = (py * w + px) * 3;
                        p[i+0] = p[i+1] = p[i+2] = v;
                    }
                }
    };
    circle(80  + (frame % 20),  60,  22, 230);
    circle(200 - (frame % 15), 110,  16, 240);
    circle(260,                180 + (frame % 10), 13, 210);
    return img;
}

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto img = make_test_image(frame);
    VAR(input, img);

    cv::Mat gm;
    cv::cvtColor(img.as_cv_mat(), gm, cv::COLOR_RGB2GRAY);
    xi::Image gray(gm.cols, gm.rows, 1, gm.data);
    VAR(gray_img, gray);

    // Call blob_analysis through the C ABI — zero knowledge of its internals
    auto result = blobs.process(xi::Record()
        .image("gray", gray)
        .set("threshold", (int)thresh)
        .set("min_area", (int)min_area));

    // Result is a Record with images + JSON
    VAR(detection, result);

    int count = result["blob_count"].as_int();
    VAR(blob_count, count);
    VAR(all_pass, count <= 3);

    // Per-blob stats
    for (int i = 0; i < result["blobs"].size(); ++i) {
        auto b = result["blobs"][i];
        // Access each blob's data — all safe with defaults
        int    area = b["area"].as_int();
        double cx   = b["cx"].as_double();
        double cy   = b["cy"].as_double();
        (void)area; (void)cx; (void)cy;
    }

    auto binary = result.get_image("binary");
    VAR(binary_out, binary);
}
