//
// defect_detection.cpp — example inspection using OpenCV directly.
//
// Demonstrates the camera trigger model:
// - TestImageSource runs its own acquisition thread inside the DLL
// - cam->grab() dequeues the latest frame
// - The backend calls xi_inspect_entry on each trigger
//

#include <xi/xi.hpp>
#include <xi/xi_image.hpp>
#include <xi/xi_source.hpp>

// Tunable parameters — show up in the VS Code Params panel
static xi::Param<int>    blur_radius {"blur_radius",  2,   {0, 10}};
static xi::Param<int>    thresh_val  {"thresh_val",   180, {0, 255}};
static xi::Param<int>    morph_radius{"morph_radius", 1,   {0, 5}};

// Image source — runs its own thread, generates frames at 5 FPS.
// In a real system this would be xi::Instance<GigECamera> or similar.
static auto& cam_source() {
    static xi::TestImageSource src("cam0", 320, 240, 5);
    return src;
}

// Auto-start the source when the DLL loads. Auto-stop on unload.
struct AutoStart {
    AutoStart()  { cam_source().start(); }
    ~AutoStart() { cam_source().stop(); }
} static g_auto_start;

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    (void)frame;
    xi::Image rgb = cam_source().grab_wait(500);
    if (rgb.empty()) return;

    VAR(input, rgb);

    cv::Mat src = rgb.as_cv_mat();

    cv::Mat gray;
    cv::cvtColor(src, gray, cv::COLOR_RGB2GRAY);

    cv::Mat blurred;
    int br = (int)blur_radius;
    if (br > 0) {
        int k = 2 * br + 1;
        cv::GaussianBlur(gray, blurred, cv::Size(k, k), 0);
    } else {
        blurred = gray;
    }

    cv::Mat binary;
    cv::threshold(blurred, binary, (int)thresh_val, 255, cv::THRESH_BINARY);

    cv::Mat cleaned;
    int mr = (int)morph_radius;
    if (mr > 0) {
        int k = 2 * mr + 1;
        auto kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(k, k));
        cv::morphologyEx(binary, cleaned, cv::MORPH_OPEN, kernel);
    } else {
        cleaned = binary;
    }

    cv::Mat edges;
    cv::Sobel(gray, edges, CV_8U, 1, 1);

    cv::Mat labels;
    int n_labels = cv::connectedComponents(cleaned, labels, 8, CV_32S);
    int blob_count = std::max(0, n_labels - 1);

    cv::Scalar mean_intensity = cv::mean(gray);

    // Wrap cv::Mat results back as xi::Image for VAR previews. Each ctor
    // does one heap copy; for hot-path code use Image::create_in_pool +
    // as_cv_mat() to write into pool memory directly (see plugin
    // examples).
    VAR(gray_img,    xi::Image(gray.cols,    gray.rows,    1, gray.data));
    VAR(blurred_img, xi::Image(blurred.cols, blurred.rows, 1, blurred.data));
    VAR(binary_img,  xi::Image(binary.cols,  binary.rows,  1, binary.data));
    VAR(cleaned_img, xi::Image(cleaned.cols, cleaned.rows, 1, cleaned.data));
    VAR(edges_img,   xi::Image(edges.cols,   edges.rows,   1, edges.data));
    VAR(blob_count_var,    blob_count);
    VAR(mean_intensity,    mean_intensity[0]);
    VAR(pass,              blob_count <= 2);
}
