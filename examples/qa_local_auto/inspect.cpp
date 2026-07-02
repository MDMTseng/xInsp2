// qa_local_auto — the reused local_image_source in AUTO mode (auto_ms>0) self-emits
// the folder's images on a timer (cycling, re-scanning so dropped/edited files
// auto-update). This script reads each emitted image, surfaces it for the HMI image
// card, and emits a brightness-based verdict.
#include <xi/xi.hpp>
#include <xi/xi_cv.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>
#include <opencv2/opencv.hpp>

XI_INSPECT_ENTRY(t, /*frame*/ frame) {
    (void)frame;
    if (!t.is_active()) return;                 // ignore any non-source tick

    xi::Image img = t.image("frame");           // source emits Record().image("frame", img)
    if (img.empty()) { xi::ng(2, "no image"); return; }

    cv::Mat m = xi::as_cv_mat(img);
    double brightness = cv::mean(m)[0] / 255.0;

    // Surface the frame image + brightness through `expose` (channel "auto").
    // The HMI image card binds the "frame" image key.
    xi::use("expose").process(xi::Record()
        .set("$channel", "auto")
        .image("frame", img)
        .set("brightness", brightness));

    if (brightness > 0.15 && brightness < 0.85) xi::ok(1, "brightness ok");
    else                                        xi::ng(1, "brightness out of range");
}
