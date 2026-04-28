//
// record_demo.cpp — demonstrates Record-based output with grouped data.
//

#include <xi/xi.hpp>
#include <xi/xi_image.hpp>
#include <xi/xi_record.hpp>

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
    cv::Mat gm;
    cv::cvtColor(img.as_cv_mat(), gm, cv::COLOR_RGB2GRAY);
    xi::Image gray(gm.cols, gm.rows, 1, gm.data);

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

        // Crop ROI from gray and run a small cv:: pipeline.
        cv::Mat roi_mat = gm(cv::Rect(rx, ry, rw, rh)).clone();
        cv::Mat edges_mat;
        cv::Sobel(roi_mat, edges_mat, CV_8U, 1, 1);
        cv::Mat bin_mat;
        cv::threshold(roi_mat, bin_mat, (int)thresh, 255, cv::THRESH_BINARY);

        cv::Mat labels;
        int n_labels = cv::connectedComponents(bin_mat, labels, 8, CV_32S);
        int blobs = std::max(0, n_labels - 1);

        xi::Image roi(roi_mat.cols, roi_mat.rows, 1, roi_mat.data);
        xi::Image edges(edges_mat.cols, edges_mat.rows, 1, edges_mat.data);
        xi::Image bin(bin_mat.cols, bin_mat.rows, 1, bin_mat.data);
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
