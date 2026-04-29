//
// inspect.cpp — drive count_or_crash on a frame loaded from disk.
//
// The driver passes frame_path to c.run(); we decode it, convert to gray
// (decoder yields RGB, not BGR), and call the counter plugin. If the plugin
// crashed in its isolated worker, the returned Record carries `error`; we
// surface that as a VAR so the driver can detect the absorbed crash.
//

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto path = xi::current_frame_path();
    VAR(frame_path, path);
    if (path.empty()) {
        VAR(error, std::string("no frame_path provided"));
        VAR(count, int(-1));
        VAR(crashed, false);
        return;
    }

    auto rgb = xi::imread(path);
    if (rgb.empty()) {
        VAR(error, std::string("imread failed: ") + path);
        VAR(count, int(-1));
        VAR(crashed, false);
        return;
    }

    cv::Mat src_mat = rgb.as_cv_mat();
    cv::Mat gray_mat;
    if (rgb.channels == 1) {
        gray_mat = src_mat;
    } else {
        cv::cvtColor(src_mat, gray_mat, cv::COLOR_RGB2GRAY);
    }
    xi::Image gray(gray_mat.cols, gray_mat.rows, 1, gray_mat.data);

    auto& counter = xi::use("counter");
    auto out = counter.process(xi::Record().image("src", gray));

    VAR(error,   out["error"].as_string(""));
    VAR(crashed, !error.empty());
    VAR(count,   out["count"].as_int(-1));
}
