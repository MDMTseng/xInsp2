//
// use_demo.cpp — demonstrates xi::use() for backend-managed instances.
//
// This is the RECOMMENDED pattern. Instances are created via VS Code UI
// or cmd:create_instance. The script accesses them by name via xi::use().
// Hot-reload does NOT destroy instances or their state.
//

#include <xi/xi.hpp>
#include <xi/xi_record.hpp>
#include <xi/xi_use.hpp>

xi::Param<int> thresh{"threshold", 128, {0, 255}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    // Access backend-managed camera — survives hot-reload
    auto& cam = xi::use("cam0");
    auto img = cam.grab(500);

    if (img.empty()) {
        // No camera frame — generate a test image
        img = xi::Image(320, 240, 3);
        uint8_t* p = img.data();
        for (int y = 0; y < 240; ++y)
            for (int x = 0; x < 320; ++x) {
                int i = (y * 320 + x) * 3;
                p[i+0] = (uint8_t)((x + frame * 3) & 0xFF);
                p[i+1] = (uint8_t)((y + frame * 5) & 0xFF);
                p[i+2] = (uint8_t)(100);
            }
    }

    VAR(input, img);
    cv::Mat gray_mat;
    cv::cvtColor(img.as_cv_mat(), gray_mat, cv::COLOR_RGB2GRAY);
    cv::Mat bin;
    cv::threshold(gray_mat, bin, (int)thresh, 255, cv::THRESH_BINARY);

    // Wrap the cv::Mat results back as xi::Image for VAR previews. The
    // ctor does a one-shot copy into a heap buffer; for hot-path code
    // prefer Image::create_in_pool() and write cv:: output directly.
    xi::Image gray(gray_mat.cols, gray_mat.rows, 1, gray_mat.data);
    xi::Image binary(bin.cols, bin.rows, 1, bin.data);
    VAR(gray, gray);
    VAR(binary, binary);

    cv::Mat labels;
    int n_labels = cv::connectedComponents(bin, labels, 8, CV_32S);
    int blobs = std::max(0, n_labels - 1);
    VAR(blob_count, blobs);
    VAR(pass, blobs <= 5);

    // Persistent state — survives hot-reload
    int count = xi::state()["run_count"].as_int(0);
    xi::state().set("run_count", count + 1);
    xi::state().set("last_blob_count", blobs);

    VAR(run_count, count + 1);
}
