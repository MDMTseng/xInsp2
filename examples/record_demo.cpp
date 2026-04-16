//
// record_demo.cpp — demonstrates Record-based output with grouped data.
//

#include <xi/xi.hpp>
#include <xi/xi_image.hpp>
#include <xi/xi_ops.hpp>
#include <xi/xi_record.hpp>

using namespace xi::ops;

xi::Param<int> thresh{"threshold", 150, {0, 255}};

static xi::Image make_test_image(int frame, int w, int h) {
    xi::Image img(w, h, 3);
    uint8_t* p = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int i = (y * w + x) * 3;
            p[i+0] = (uint8_t)((x * 200 / w + frame * 3) & 0xFF);
            p[i+1] = (uint8_t)((y * 180 / h + frame * 5) & 0xFF);
            p[i+2] = (uint8_t)(100 + (frame & 0x3F));
        }
    return img;
}

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto img = make_test_image(frame, 320, 240);
    auto gray = toGray(img);

    // Step 1: preprocess — record with images + metadata
    VAR(preprocess, xi::Record()
        .image("input", img)
        .image("gray", gray)
        .set("width", img.width)
        .set("height", img.height)
        .set("frame", frame));

    // Step 2: detection — simulate 4 ROI matches
    xi::Record results;
    results.set("total", 4);
    int pass_count = 0;

    for (int i = 0; i < 4; ++i) {
        int rx = (i % 2) * 160;
        int ry = (i / 2) * 120;
        int rw = 160, rh = 120;

        // Crop ROI from gray
        xi::Image roi(rw, rh, 1);
        for (int y = 0; y < rh; ++y)
            for (int x = 0; x < rw; ++x)
                roi.data()[y * rw + x] = gray.data()[(ry + y) * gray.width + (rx + x)];

        auto edges = sobel(roi);
        auto bin   = threshold(roi, static_cast<int>(thresh));
        int  blobs = countWhiteBlobs(bin);
        bool pass  = (blobs <= 3);
        if (pass) pass_count++;
        double score = 100.0 - blobs * 15.0;

        results.push("items", xi::Record()
            .image("roi", roi)
            .image("edges", edges)
            .image("binary", bin)
            .set("x", rx)
            .set("y", ry)
            .set("blobs", blobs)
            .set("score", score)
            .set("pass", pass));
    }

    results.set("pass_count", pass_count);
    results.set("all_pass", pass_count == 4);
    VAR(detection, results);

    // Step 3: summary
    VAR(overall_pass, pass_count == 4);
    VAR(score_avg, 85.5);
}
