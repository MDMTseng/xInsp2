// qa_local_auto — the reused local_image_source in AUTO mode (auto_ms>0) self-emits
// the folder's images on a timer (cycling, re-scanning so dropped/edited files
// auto-update). This script reads each emitted image, surfaces it for the HMI image
// card, and emits a brightness-based verdict.
#include <xi/xi.hpp>
#include <xi/xi_cv.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>
#include <opencv2/opencv.hpp>

XI_INSPECT_ENTRY(t, /*frame*/ frame) {
    (void)frame;
    if (!t.is_active()) return;                 // ignore any non-source tick

    auto p = t.pack();                          // source emits a pack keyed "frame"
    if (!p) { xi::ng(2, "no pack"); return; }
    auto pv = p.get_image("frame");
    if (!pv) { xi::ng(2, "no image"); return; }

    cv::Mat m((int)pv->height, (int)pv->width, CV_8UC((int)pv->channels),
              (void*)pv->pixels.data());
    double brightness = cv::mean(m)[0] / 255.0;

    // Surface the frame image + brightness through `expose` (channel "auto").
    // The HMI image card binds the "frame" image key.
    xi::ScriptPackBuilder b;
    b.add_str("$channel", "auto");
    b.add_image("frame", (int)pv->width, (int)pv->height, (int)pv->channels,
                pv->pixels.data());
    b.add_f64("brightness", brightness);
    xi::use("expose").push(b.seal());

    if (brightness > 0.15 && brightness < 0.85) xi::ok(1, "brightness ok");
    else                                        xi::ng(1, "brightness out of range");
}
