//
// hmi_demo — a source-less script that synthesises a live stream for the HMI:
// a verdict (OK/NG), a metric (fg_pct, slow sine), and an image (moving dot).
// Run continuous (the HMI runner uses --autostart-fps) and point the HMI at it.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>
#include <cmath>
#include <string>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    // Metric: a slow sine that occasionally drifts out of the [0.4, 0.6] band.
    const double m = 0.5 + 0.13 * std::sin(frame * 0.13);
    VAR(fg_pct, m);

    const bool ok = (m > 0.4 && m < 0.6);
    VAR(verdict, std::string(ok ? "OK" : "NG"));

    // The one per-run verdict the HMI verdict/yield cards consume. Two distinct
    // ng sub-codes so the verdict card's message line shows *why* it failed.
    if (ok)            xi::ok(1, "fg in band");
    else if (m <= 0.4) xi::ng(1, "fg too low");
    else               xi::ng(2, "fg too high");

    // Synthetic image: a dot orbiting on a dark field, coloured by the verdict.
    cv::Mat bgr(240, 320, CV_8UC3, cv::Scalar(24, 24, 24));
    const int cx = 160 + (int)std::lround(110 * std::cos(frame * 0.10));
    const int cy = 120 + (int)std::lround(70  * std::sin(frame * 0.10));
    cv::circle(bgr, {cx, cy}, 26, ok ? cv::Scalar(70, 200, 70) : cv::Scalar(70, 70, 230), -1);
    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);   // xi::Image is RGB
    VAR(result, xi::from_cv_mat(rgb));
}
