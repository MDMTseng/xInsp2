// OQ-9 counterpart — a CV-USING translation unit.
//
// Opts into OpenCV via <xi/xi_cv.hpp> and uses the free bridge functions
// xi::as_cv_mat / xi::from_cv_mat. This one DOES need OpenCV on the include
// path (that is exactly the opt-in contract). Confirms the cv path still works.
//
#include <xi/xi.hpp>
#include <xi/xi_cv.hpp>   // pulls OpenCV + xi::as_cv_mat / from_cv_mat

#include <opencv2/imgproc.hpp>
#include <cstdio>

int main() {
    xi::Image src(8, 8, 1);
    src.data()[0] = 255;

    cv::Mat m = xi::as_cv_mat(src);           // zero-copy view
    cv::Mat blurred;
    cv::GaussianBlur(m, blurred, cv::Size(3, 3), 0);

    xi::Image out = xi::from_cv_mat(blurred);  // copy back to owning Image
    if (out.empty()) return 1;

    std::printf("oq9 cv opt-in OK: out=%dx%dx%d\n",
                out.width, out.height, out.channels);
    return 0;
}
