//
// inspect.cpp — object_count_puzzle
//
// Detect DARK roughly-circular blobs on a LIGHT, unevenly-illuminated and
// progressively-degraded background. Runs entirely inside the xInsp2 script
// via xi::Image + its cv::Mat view (the sanctioned op path — see xi_image.hpp
// which demonstrates calling cv:: directly on as_cv_mat()).
//
// Pipeline (all on the cv::Mat view of the xi::Image):
//   1. read frame, force single-channel gray
//   2. median blur            -> kill salt & pepper
//   3. gaussian blur          -> smooth additive gaussian noise
//   4. illumination flatten   -> large-kernel background estimate, then
//                                background - signal (so dark blobs become
//                                bright positive bumps independent of the
//                                vignette / gradient)
//   5. Otsu threshold on the flattened response  -> binary mask
//   6. morphology open then close -> remove specks, fill blobs
//   7. connected components with stats -> area + extent (fill ratio) filter
//   8. centroids of surviving components
//
// Surfaces (via the `expose` plugin, channel "objects"): count (int),
// centroids ("[[x,y],...]") and a mask preview image.
//

#include <xi/xi.hpp>
#include <xi/xi_cv.hpp>
#include <xi/xi_use.hpp>

#include <string>
#include <vector>
#include <cstdio>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto path = xi::current_frame_path();
    if (path.empty()) {
        xi::use("expose").process(xi::Record()
            .set("$channel", "error")
            .set("error", std::string("no frame_path supplied to cmd:run"))
            .set("count", -1));
        return;
    }

    auto frame = xi::imread(path);
    if (frame.empty()) {
        xi::use("expose").process(xi::Record()
            .set("$channel", "error")
            .set("error", std::string("frame load failed: ") + path)
            .set("count", -1));
        return;
    }

    // --- to single-channel gray ---
    cv::Mat src = xi::as_cv_mat(frame);
    cv::Mat gray;
    if (frame.channels == 1) {
        gray = src.clone();
    } else if (frame.channels == 3) {
        cv::cvtColor(src, gray, cv::COLOR_RGB2GRAY);
    } else if (frame.channels == 4) {
        cv::cvtColor(src, gray, cv::COLOR_RGBA2GRAY);
    } else {
        gray = src.clone();
    }

    // --- 2. median blur: remove salt & pepper impulse noise ---
    // Two passes of a 5px median scrub impulse noise hard; on harsh frames a
    // single pass leaves clumps that survive downstream as false blobs.
    cv::Mat med;
    cv::medianBlur(gray, med, 5);
    cv::medianBlur(med, med, 5);

    // --- 3. gaussian blur: smooth additive gaussian noise ---
    // Blobs are radius ~10-22px, so a sigma ~5 gaussian barely touches a real
    // blob's core but averages random noise toward the local mean. This is the
    // key denoise that lets the harsh frames survive the flatten + threshold.
    cv::Mat blur;
    cv::GaussianBlur(med, blur, cv::Size(0, 0), 5.0);

    // --- 4. illumination flatten ---
    // Estimate the smoothly-varying background with a very large blur. Blobs
    // (radius ~10-22px) vanish under a ~81px gaussian, so `bg` is essentially
    // the illumination field. bg - blur is then a flat-field response that is
    // POSITIVE where the image is darker than its local background (i.e. on a
    // dark blob) and ~0 elsewhere, regardless of the vignette / gradient.
    cv::Mat bg;
    cv::GaussianBlur(blur, bg, cv::Size(0, 0), 31.0);
    cv::Mat respf, resp;
    cv::subtract(bg, blur, respf, cv::noArray(), CV_32F); // bg - signal, signed
    cv::max(respf, 0.0, respf);                            // keep dark-blob bumps
    double mn, mx;
    cv::minMaxLoc(respf, &mn, &mx);
    if (mx < 1e-6) {
        xi::use("expose").process(xi::Record()
            .set("$channel", "objects")
            .set("frame_path", path)
            .set("count", 0)
            .set("centroids", std::string("[]")));
        return;
    }
    respf.convertTo(resp, CV_8U, 255.0 / mx);             // normalize to 0..255

    // --- 5. Otsu threshold on the flattened response ---
    cv::Mat bin;
    double otsu_level = cv::threshold(resp, bin, 0, 255,
                                cv::THRESH_BINARY | cv::THRESH_OTSU);
    // Guard against Otsu firing on a near-empty response (pure noise frames):
    // if the chosen level is tiny the "signal" is just residual noise. Require
    // a minimum absolute contrast in the flattened response.
    if (otsu_level < 18.0) {
        // re-threshold at a fixed conservative floor instead of Otsu's noise level
        cv::threshold(resp, bin, 40, 255, cv::THRESH_BINARY);
    }

    // --- 6. morphology: open (despeckle) then close (fill blob bodies) ---
    cv::Mat k3 = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::Mat k5 = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
    cv::Mat mask;
    cv::morphologyEx(bin, mask, cv::MORPH_OPEN, k3);
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, k5);

    // --- 7. contours + per-blob shape filter ---
    // Blobs have radius ~10-22 -> area ~314..1520. A strongly-blurred real blob
    // stays well above ~150px; noise clumps that survive the median+gaussian are
    // smaller and irregular, so area + circularity together reject them.
    const double MIN_AREA = 150.0;
    const double MAX_AREA = 4000.0;
    const double MIN_EXTENT = 0.55;       // filled / bbox; a disc ~0.785
    const double MIN_CIRCULARITY = 0.62;  // 4*pi*A / P^2; a perfect disc = 1.0

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);

    std::string out = "[";
    int nblob = 0;
    for (auto& cnt : contours) {
        double area = cv::contourArea(cnt);
        if (area < MIN_AREA || area > MAX_AREA) continue;

        cv::Rect bb = cv::boundingRect(cnt);
        double bbox = double(bb.width) * double(bb.height);
        double extent = bbox > 0 ? area / bbox : 0.0;
        if (extent < MIN_EXTENT) continue;

        double ar = (bb.height > 0) ? double(bb.width) / double(bb.height) : 0.0;
        if (ar < 0.45 || ar > 2.2) continue;

        double perim = cv::arcLength(cnt, true);
        double circ = perim > 0 ? 4.0 * CV_PI * area / (perim * perim) : 0.0;
        if (circ < MIN_CIRCULARITY) continue;

        cv::Moments m = cv::moments(cnt);
        if (m.m00 <= 0) continue;
        double cx = m.m10 / m.m00;
        double cy = m.m01 / m.m00;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s[%.1f,%.1f]",
                      nblob ? "," : "", cx, cy);
        out += buf;
        ++nblob;
    }
    out += "]";

    // Build a uint8 mask Image for preview, then surface everything through the
    // `expose` plugin (channel "objects").
    xi::Record rec;
    rec.set("$channel", "objects");
    rec.set("frame_path", path);

    xi::Image maskImg(mask.cols, mask.rows, 1);
    if (!maskImg.empty()) {
        cv::Mat mv = xi::as_cv_mat(maskImg);
        mask.copyTo(mv);
        rec.image("maskpreview", maskImg);
    }

    rec.set("count", nblob);
    rec.set("centroids", out);
    rec.set("otsu", double(otsu_level));
    xi::use("expose").process(rec);
}
