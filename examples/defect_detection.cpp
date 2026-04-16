//
// defect_detection.cpp — example inspection using xi::ops.
//
// Generates a synthetic test image with "defects" (bright spots),
// processes it through the standard pipeline, counts blobs.
//

#include <xi/xi.hpp>
#include <xi/xi_image.hpp>
#include <xi/xi_ops.hpp>
#include <xi/xi_source.hpp>

using namespace xi::ops;

// Tunable parameters — show up as sliders in the VS Code UI
static xi::Param<int>    blur_radius {"blur_radius",  2,   {0, 10}};
static xi::Param<int>    thresh_val  {"thresh_val",   180, {0, 255}};
static xi::Param<int>    morph_radius{"morph_radius", 1,   {0, 5}};

// Image source — generates synthetic test images with random-ish "defects"
static xi::Instance<xi::TestImageSource> cam{"cam0"};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    // Grab latest frame from the source
    xi::Image rgb = cam->grab();
    if (rgb.empty()) {
        // In single-shot mode, generate a test image
        rgb = xi::Image(320, 240, 3);
        uint8_t* p = rgb.data();
        for (int y = 0; y < 240; ++y) {
            for (int x = 0; x < 320; ++x) {
                int i = (y * 320 + x) * 3;
                // Background gradient
                p[i + 0] = static_cast<uint8_t>((x * 180 / 320 + frame) & 0xFF);
                p[i + 1] = static_cast<uint8_t>((y * 150 / 240 + frame) & 0xFF);
                p[i + 2] = static_cast<uint8_t>(100);
                // Add bright spots as "defects"
                int cx1 = 80 + (frame * 3) % 60;
                int cy1 = 60 + (frame * 7) % 40;
                int cx2 = 200 + (frame * 5) % 50;
                int cy2 = 150 + (frame * 11) % 30;
                if ((x-cx1)*(x-cx1) + (y-cy1)*(y-cy1) < 400) { p[i] = 255; p[i+1] = 255; p[i+2] = 255; }
                if ((x-cx2)*(x-cx2) + (y-cy2)*(y-cy2) < 300) { p[i] = 240; p[i+1] = 240; p[i+2] = 240; }
            }
        }
    }
    VAR(input, rgb);

    // Standard inspection pipeline
    VAR(gray,     toGray(input));
    VAR(blurred,  gaussian(gray, blur_radius));
    VAR(binary,   threshold(blurred, thresh_val));
    VAR(cleaned,  dilate(erode(binary, morph_radius), morph_radius));

    // Parallel: edge detection + blob counting
    auto p_edges = async_sobel(gray);
    auto p_blobs = async_countWhiteBlobs(cleaned);

    VAR(edges,      xi::Image(p_edges));
    VAR(blob_count, int(p_blobs));

    // Stats on the grayscale
    auto st = stats(gray);
    VAR(mean_intensity, st.mean);
    VAR(pass, blob_count <= 2);
}
