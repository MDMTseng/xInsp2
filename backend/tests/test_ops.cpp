//
// test_ops.cpp — unit tests for xi::ops image operators.
//
// Covers all 17 tests from TEST_PLAN.md section "test_ops.cpp (NEW)".
// Uses the same CHECK/SECTION pattern as test_xi_core.cpp.
//

#include <cmath>
#include <cstdio>
#include <cstring>

#include <xi/xi_image.hpp>
#include <xi/xi_ops.hpp>

// Minimal test harness (same as test_xi_core.cpp)
static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define SECTION(name) std::printf("[test] %s\n", name)

// ---------- helpers ----------

// Create a grayscale image filled with a single value.
static xi::Image make_gray(int w, int h, uint8_t fill) {
    xi::Image img(w, h, 1);
    std::memset(img.data(), fill, static_cast<size_t>(w) * h);
    return img;
}

// Create an RGB image where every pixel has the same (r,g,b).
static xi::Image make_rgb(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    xi::Image img(w, h, 3);
    uint8_t* p = img.data();
    for (int i = 0; i < w * h; ++i) {
        p[i * 3 + 0] = r;
        p[i * 3 + 1] = g;
        p[i * 3 + 2] = b;
    }
    return img;
}

// ---------- toGray tests ----------

static void test_toGray_white() {
    SECTION("toGray white -> 255");
    auto rgb = make_rgb(1, 1, 255, 255, 255);
    auto gray = xi::ops::toGray(rgb);
    CHECK(!gray.empty());
    CHECK(gray.channels == 1);
    CHECK(gray.width == 1);
    CHECK(gray.height == 1);
    // BT.601: (255*77 + 255*150 + 255*29) >> 8 = 255*256/256 = 255
    CHECK(gray.data()[0] == 255);
}

static void test_toGray_black() {
    SECTION("toGray black -> 0");
    auto rgb = make_rgb(1, 1, 0, 0, 0);
    auto gray = xi::ops::toGray(rgb);
    CHECK(!gray.empty());
    CHECK(gray.data()[0] == 0);
}

static void test_toGray_already_gray() {
    SECTION("toGray already gray -> same");
    auto src = make_gray(4, 4, 128);
    auto dst = xi::ops::toGray(src);
    CHECK(!dst.empty());
    CHECK(dst.channels == 1);
    CHECK(dst.width == 4);
    CHECK(dst.height == 4);
    // Passthrough: every pixel should remain 128
    for (int i = 0; i < 16; ++i) {
        CHECK(dst.data()[i] == 128);
    }
}

// ---------- threshold tests ----------

static void test_threshold_boundary() {
    SECTION("threshold at boundary");
    // Create a 4x4 gray image with values 100 and 101
    xi::Image src(4, 4, 1);
    uint8_t* p = src.data();
    for (int i = 0; i < 16; ++i) {
        p[i] = (i % 2 == 0) ? 100 : 101;
    }
    auto dst = xi::ops::threshold(src, 100);
    const uint8_t* dp = dst.data();
    for (int i = 0; i < 16; ++i) {
        if (i % 2 == 0) {
            // value == t (100) -> 0 (not strictly greater)
            CHECK(dp[i] == 0);
        } else {
            // value == t+1 (101) -> 255
            CHECK(dp[i] == 255);
        }
    }
}

static void test_threshold_invert() {
    SECTION("threshold invert (via custom max_val)");
    // Use max_val=0 to get inverted output compared to max_val=255
    auto src = make_gray(4, 4, 200);
    // threshold with t=100, max_val=255: 200>100 -> 255
    auto normal = xi::ops::threshold(src, 100, 255);
    CHECK(normal.data()[0] == 255);
    // threshold with t=100, max_val=0: 200>100 -> 0 (custom max_val)
    auto inv = xi::ops::threshold(src, 100, 0);
    CHECK(inv.data()[0] == 0);
}

// ---------- invert tests ----------

static void test_invert_values() {
    SECTION("invert 0 -> 255, 255 -> 0");
    xi::Image src(2, 2, 1);
    uint8_t* p = src.data();
    p[0] = 0;
    p[1] = 255;
    p[2] = 128;
    p[3] = 1;
    auto dst = xi::ops::invert(src);
    CHECK(dst.data()[0] == 255);
    CHECK(dst.data()[1] == 0);
    CHECK(dst.data()[2] == 127);
    CHECK(dst.data()[3] == 254);
}

static void test_invert_roundtrip() {
    SECTION("invert round-trip");
    xi::Image src(4, 4, 1);
    uint8_t* p = src.data();
    for (int i = 0; i < 16; ++i) p[i] = static_cast<uint8_t>(i * 17);
    auto dst = xi::ops::invert(xi::ops::invert(src));
    for (int i = 0; i < 16; ++i) {
        CHECK(dst.data()[i] == src.data()[i]);
    }
}

// ---------- sobel tests ----------

static void test_sobel_flat() {
    SECTION("sobel flat image -> all zeros");
    auto src = make_gray(8, 8, 128);
    auto dst = xi::ops::sobel(src);
    CHECK(!dst.empty());
    // All pixels should be 0 (no edges in a flat image)
    for (int i = 0; i < 8 * 8; ++i) {
        CHECK(dst.data()[i] == 0);
    }
}

static void test_sobel_single_edge() {
    SECTION("sobel single edge -> nonzero");
    // Create an 8x8 image with a vertical edge: left half black, right half white
    xi::Image src(8, 8, 1);
    uint8_t* p = src.data();
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            p[y * 8 + x] = (x < 4) ? 0 : 255;
        }
    }
    auto dst = xi::ops::sobel(src);
    // The interior pixels at the edge boundary (x=3,4 for y=1..6) should be nonzero
    bool found_nonzero = false;
    for (int y = 1; y < 7; ++y) {
        for (int x = 3; x <= 4; ++x) {
            if (dst.data()[y * 8 + x] > 0) {
                found_nonzero = true;
            }
        }
    }
    CHECK(found_nonzero);
}

// ---------- erode / dilate tests ----------

static void test_erode_shrinks() {
    SECTION("erode shrinks white region");
    // 5x5 image, 3x3 white square in center, rest black
    xi::Image src(5, 5, 1);
    std::memset(src.data(), 0, 25);
    for (int y = 1; y <= 3; ++y) {
        for (int x = 1; x <= 3; ++x) {
            src.data()[y * 5 + x] = 255;
        }
    }
    auto dst = xi::ops::erode(src, 1);
    // After erode(1), only the center pixel (2,2) should remain white
    // because it is the only pixel with all neighbors white
    CHECK(dst.data()[2 * 5 + 2] == 255);
    // Corner of the original white square should now be 0
    CHECK(dst.data()[1 * 5 + 1] == 0);
    CHECK(dst.data()[1 * 5 + 3] == 0);
    CHECK(dst.data()[3 * 5 + 1] == 0);
    CHECK(dst.data()[3 * 5 + 3] == 0);
}

static void test_dilate_expands() {
    SECTION("dilate expands white region");
    // 5x5 image, single white pixel at center (2,2)
    xi::Image src(5, 5, 1);
    std::memset(src.data(), 0, 25);
    src.data()[2 * 5 + 2] = 255;
    auto dst = xi::ops::dilate(src, 1);
    // After dilate(1), the 3x3 region around center should be white
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int y = 2 + dy, x = 2 + dx;
            CHECK(dst.data()[y * 5 + x] == 255);
        }
    }
    // Corners of the 5x5 should remain black
    CHECK(dst.data()[0 * 5 + 0] == 0);
    CHECK(dst.data()[0 * 5 + 4] == 0);
    CHECK(dst.data()[4 * 5 + 0] == 0);
    CHECK(dst.data()[4 * 5 + 4] == 0);
}

// ---------- countWhiteBlobs tests ----------

static void test_countWhiteBlobs_zero() {
    SECTION("countWhiteBlobs 0 blobs");
    auto src = make_gray(4, 4, 0);
    int count = xi::ops::countWhiteBlobs(src);
    CHECK(count == 0);
}

static void test_countWhiteBlobs_one() {
    SECTION("countWhiteBlobs 1 blob");
    // Single connected region: 2x2 block of white pixels
    xi::Image src(4, 4, 1);
    std::memset(src.data(), 0, 16);
    src.data()[0 * 4 + 0] = 255;
    src.data()[0 * 4 + 1] = 255;
    src.data()[1 * 4 + 0] = 255;
    src.data()[1 * 4 + 1] = 255;
    int count = xi::ops::countWhiteBlobs(src);
    CHECK(count == 1);
}

static void test_countWhiteBlobs_three() {
    SECTION("countWhiteBlobs 3 blobs");
    // Three separated single-pixel dots on a 6x6 image
    xi::Image src(6, 6, 1);
    std::memset(src.data(), 0, 36);
    src.data()[0 * 6 + 0] = 255;  // blob 1 at (0,0)
    src.data()[0 * 6 + 5] = 255;  // blob 2 at (5,0)
    src.data()[5 * 6 + 3] = 255;  // blob 3 at (3,5)
    int count = xi::ops::countWhiteBlobs(src);
    CHECK(count == 3);
}

// ---------- stats test ----------

static void test_stats_known() {
    SECTION("stats on known image");
    // 4-pixel image with values: 10, 20, 30, 40
    xi::Image src(2, 2, 1);
    src.data()[0] = 10;
    src.data()[1] = 20;
    src.data()[2] = 30;
    src.data()[3] = 40;
    auto s = xi::ops::stats(src);
    CHECK(s.pixel_count == 4);
    // mean = (10+20+30+40)/4 = 25.0
    CHECK(std::abs(s.mean - 25.0) < 0.01);
    CHECK(s.min_val == 10);
    CHECK(s.max_val == 40);
    // stddev = sqrt((100+400+900+1600)/4 - 625) = sqrt(750-625) = sqrt(125) ~= 11.18
    // Actually: sum2 = 10^2+20^2+30^2+40^2 = 100+400+900+1600 = 3000
    // variance = 3000/4 - 25^2 = 750 - 625 = 125
    // stddev = sqrt(125) = 11.180...
    CHECK(std::abs(s.stddev - std::sqrt(125.0)) < 0.01);
}

// ---------- gaussian edge case ----------

static void test_gaussian_1x1() {
    SECTION("gaussian doesn't crash on 1x1");
    auto src = make_gray(1, 1, 200);
    auto dst = xi::ops::gaussian(src, 3);
    CHECK(!dst.empty());
    CHECK(dst.width == 1);
    CHECK(dst.height == 1);
    // With a 1x1 image and clamped border, the single pixel should be preserved
    CHECK(dst.data()[0] == 200);
}

// ---------- boxBlur radius=0 ----------

static void test_boxBlur_radius0() {
    SECTION("boxBlur radius=0 -> identity");
    xi::Image src(4, 4, 1);
    for (int i = 0; i < 16; ++i) src.data()[i] = static_cast<uint8_t>(i * 17);
    auto dst = xi::ops::boxBlur(src, 0);
    CHECK(!dst.empty());
    for (int i = 0; i < 16; ++i) {
        CHECK(dst.data()[i] == src.data()[i]);
    }
}

// ---------- open / close ----------

static void test_open_removes_speck() {
    SECTION("open removes a 1-pixel speck on black background");
    xi::Image src(7, 7, 1);
    std::memset(src.data(), 0, 49);
    src.data()[3 * 7 + 3] = 255;             // isolated speck
    auto dst = xi::ops::open(src, 1);
    CHECK(dst.data()[3 * 7 + 3] == 0);       // speck gone
}

static void test_close_fills_hole() {
    SECTION("close fills a 1-pixel hole inside a white block");
    xi::Image src(7, 7, 1);
    std::memset(src.data(), 255, 49);
    src.data()[3 * 7 + 3] = 0;                // isolated hole
    auto dst = xi::ops::close(src, 1);
    CHECK(dst.data()[3 * 7 + 3] == 255);      // hole filled
}

// ---------- adaptive threshold ----------

static void test_adaptive_threshold_gradient() {
    SECTION("adaptive threshold handles a lighting gradient that fixed thresh can't");
    // 20x20 gradient 50→150 across x, with a local bump at (10,10)
    int W = 20, H = 20;
    xi::Image src(W, H, 1);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            src.data()[y * W + x] = (uint8_t)(50 + x * 5);
    // Local bump: +40 at one spot
    src.data()[10 * W + 10] = (uint8_t)(50 + 10 * 5 + 40);
    auto dst = xi::ops::adaptiveThreshold(src, 3, 10);
    // The bump should stand out even though it's below the max gradient value
    CHECK(dst.data()[10 * W + 10] == 255);
}

// ---------- canny ----------

static void test_canny_blank() {
    SECTION("canny on a uniform image yields no edges");
    auto src = make_gray(20, 20, 128);
    auto dst = xi::ops::canny(src, 30, 80);
    int edge_count = 0;
    for (int i = 0; i < dst.size(); ++i) if (dst.data()[i]) ++edge_count;
    CHECK(edge_count == 0);
}

static void test_canny_finds_edge() {
    SECTION("canny on a sharp step detects the edge");
    xi::Image src(20, 20, 1);
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 20; ++x)
            src.data()[y * 20 + x] = (uint8_t)(x < 10 ? 30 : 220);
    auto dst = xi::ops::canny(src, 30, 80);
    int edge_count = 0;
    for (int i = 0; i < dst.size(); ++i) if (dst.data()[i]) ++edge_count;
    CHECK(edge_count > 5);
    CHECK(edge_count < 40);          // canny should produce a thin ridge, not fill
}

// ---------- contours + bbox ----------

static void test_findFilledRegions_two_blobs() {
    SECTION("findFilledRegions returns every pixel of each component");
    xi::Image bin(20, 20, 1);
    std::memset(bin.data(), 0, 400);
    for (int y = 2; y <= 5; ++y)
        for (int x = 2; x <= 5; ++x) bin.data()[y*20 + x] = 255;
    for (int y = 12; y <= 14; ++y)
        for (int x = 12; x <= 14; ++x) bin.data()[y*20 + x] = 255;
    auto regions = xi::ops::findFilledRegions(bin);
    CHECK(regions.size() == 2);
    int total = 0;
    for (auto& r : regions) total += (int)r.size();
    CHECK(total == 16 + 9);    // 4×4 + 3×3
}

static void test_findContours_two_blobs() {
    SECTION("findContours returns one bbox per connected component");
    xi::Image bin(20, 20, 1);
    std::memset(bin.data(), 0, 400);
    // Square A at (2..5, 2..5) — 4×4
    for (int y = 2; y <= 5; ++y)
        for (int x = 2; x <= 5; ++x) bin.data()[y*20 + x] = 255;
    // Square B at (12..14, 12..14) — 3×3
    for (int y = 12; y <= 14; ++y)
        for (int x = 12; x <= 14; ++x) bin.data()[y*20 + x] = 255;
    auto contours = xi::ops::findContours(bin);
    CHECK(contours.size() == 2);
    // OpenCV returns boundary-only contours; order may differ from C++
    // fallback. Match by bbox rather than indexing.
    bool found_4x4 = false, found_3x3 = false;
    for (auto& c : contours) {
        auto b = xi::ops::bbox(c);
        if (b.x == 2  && b.y == 2  && b.w == 4 && b.h == 4) found_4x4 = true;
        if (b.x == 12 && b.y == 12 && b.w == 3 && b.h == 3) found_3x3 = true;
    }
    CHECK(found_4x4);
    CHECK(found_3x3);
}

// ---------- matchTemplate ----------

static void test_matchTemplate_finds_exact() {
    SECTION("matchTemplateSSD locates an embedded pattern with score 0");
    xi::Image src(16, 16, 1);
    for (int i = 0; i < 256; ++i) src.data()[i] = (uint8_t)((i * 7) & 0xFF);
    // Build template from src region (5..7, 3..5)
    xi::Image templ(3, 3, 1);
    for (int ty = 0; ty < 3; ++ty)
        for (int tx = 0; tx < 3; ++tx)
            templ.data()[ty * 3 + tx] = src.data()[(3 + ty) * 16 + (5 + tx)];
    auto r = xi::ops::matchTemplateSSD(src, templ);
    CHECK(r.x == 5 && r.y == 3);
    CHECK(r.score < 0.5);            // exact match → 0
}

// ---------- main ----------

int main() {
    // toGray
    test_toGray_white();
    test_toGray_black();
    test_toGray_already_gray();

    // threshold
    test_threshold_boundary();
    test_threshold_invert();

    // invert
    test_invert_values();
    test_invert_roundtrip();

    // sobel
    test_sobel_flat();
    test_sobel_single_edge();

    // morphology
    test_erode_shrinks();
    test_dilate_expands();

    // blob counting
    test_countWhiteBlobs_zero();
    test_countWhiteBlobs_one();
    test_countWhiteBlobs_three();

    // stats
    test_stats_known();

    // edge cases
    test_gaussian_1x1();
    test_boxBlur_radius0();

    // S5 operators
    test_open_removes_speck();
    test_close_fills_hole();
    test_adaptive_threshold_gradient();
    test_canny_blank();
    test_canny_finds_edge();
    test_findContours_two_blobs();
    test_findFilledRegions_two_blobs();
    test_matchTemplate_finds_exact();

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
        return 1;
    }
}
