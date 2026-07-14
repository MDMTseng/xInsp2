// test_image_blob.cpp — the package-E consumer sugar for the `xi/image`
// self-describing blob convention (spec 30):
//   * xi::read_image_blob (CV-free descriptor reader, <xi/xi_image_blob.hpp>) —
//     the dt matrix + fail-loud rejects.
//   * xi::as_cv_read(ImageBlobView) / xi::as_cv_write_blob (<xi/xi_cv.hpp>) —
//     the dt -> OpenCV type mapping, packed stride, zero-copy view, and the
//     writable mint-window flavor.
//
// The SDK accessors PackIn::image_blob / ScriptPack::image_blob are thin wrappers
// (their own get_blob + this reader), so this pins the shared logic; the door
// wiring is exercised end-to-end by the plugin fleet (blob_analysis reads via
// PackIn::image_blob in pack_pilot_test).
//
#include <xi/xi_cv.hpp>          // OpenCV + as_cv_read(ImageBlobView)/as_cv_write_blob
#include <xi/xi_image_blob.hpp>  // read_image_blob + ImageBlobView
#include <xi/xi_mp.hpp>          // build xi/image descriptors under test

#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

// Build a canonical {"t":<t>,"w":w,"h":h,"c":c,"dt":<dt>} descriptor.
static std::vector<uint8_t> desc_of(std::string_view t, int w, int h, int c,
                                    std::string_view dt) {
    xi::mp::Writer dw;
    dw.map(5);
    dw.key("t");  dw.str(t);
    dw.key("w");  dw.int_(w);
    dw.key("h");  dw.int_(h);
    dw.key("c");  dw.int_(c);
    dw.key("dt"); dw.str(dt);
    xi::mp::Bytes b = dw.take();
    return std::vector<uint8_t>(b.begin(), b.end());
}

// dt -> (elem_size, expected cv depth): the full mapping matrix.
static void test_dt_matrix() {
    struct Row { const char* dt; int elem; int depth; };
    const Row rows[] = {
        {"u8", 1, CV_8U}, {"u16", 2, CV_16U}, {"i32", 4, CV_32S},
        {"f32", 4, CV_32F}, {"f64", 8, CV_64F},
    };
    const int W = 6, H = 4, C = 3;
    for (const Row& row : rows) {
        std::vector<uint8_t> d = desc_of("xi/image", W, H, C, row.dt);
        std::vector<uint8_t> payload((size_t)W * H * C * row.elem, 0x5A);
        auto ib = xi::read_image_blob(d, payload);
        CHECK(ib.has_value(), "read_image_blob accepts a well-formed xi/image blob");
        if (!ib) continue;
        CHECK(ib->width == W && ib->height == H && ib->channels == C, "shape round-trips");
        CHECK(ib->dt == row.dt, "dt round-trips");
        CHECK(ib->elem_size() == row.elem, "elem_size matches the dtype");
        CHECK((int64_t)ib->payload.size() == (int64_t)W * H * C * row.elem,
              "payload sized w*h*c*elem");

        cv::Mat m = xi::as_cv_read(*ib);
        CHECK(!m.empty(), "as_cv_read yields a non-empty Mat");
        CHECK(m.type() == CV_MAKETYPE(row.depth, C), "cv type = MAKETYPE(depth(dt), channels)");
        CHECK(m.rows == H && m.cols == W, "cv Mat dims = h x w");
        CHECK((int)m.channels() == C, "cv channels match");
        CHECK(m.step[0] == (size_t)W * C * row.elem, "packed row stride = w*c*elem");
        CHECK(m.data == ib->payload.data(), "cv Mat is a zero-copy view over the payload");
    }
}

// Fail-loud rejects — never a silent reinterpret.
static void test_rejects() {
    const int W = 4, H = 4, C = 1;
    std::vector<uint8_t> u8pay((size_t)W * H * C, 0);

    CHECK(!xi::read_image_blob(desc_of("acme/scan", W, H, C, "u8"), u8pay).has_value(),
          "reject: t != xi/image");
    CHECK(!xi::read_image_blob(desc_of("xi/image", W, H, C, "u4"), u8pay).has_value(),
          "reject: unsupported dt");
    // Claims u16 (2x bytes) over a u8-sized payload — undersized.
    CHECK(!xi::read_image_blob(desc_of("xi/image", W, H, C, "u16"), u8pay).has_value(),
          "reject: payload shorter than w*h*c*elem");
    CHECK(!xi::read_image_blob(desc_of("xi/image", 0, H, C, "u8"), u8pay).has_value(),
          "reject: non-positive dim");
    {
        xi::mp::Writer w; w.int_(7);                 // a bare int, not a map
        xi::mp::Bytes b = w.take();
        std::vector<uint8_t> notmap(b.begin(), b.end());
        CHECK(!xi::read_image_blob(notmap, u8pay).has_value(),
              "reject: descriptor is not a map");
    }
    CHECK(xi::cv_depth_for_dt("u4") == -1, "cv_depth_for_dt rejects an unknown dtype");

    // A toolbox's extra descriptor key is skipped, not rejected.
    {
        xi::mp::Writer dw;
        dw.map(6);
        dw.key("t");     dw.str("xi/image");
        dw.key("w");     dw.int_(W);
        dw.key("h");     dw.int_(H);
        dw.key("c");     dw.int_(C);
        dw.key("dt");    dw.str("u8");
        dw.key("stride"); dw.int_(999);              // unknown convention key
        xi::mp::Bytes b = dw.take();
        std::vector<uint8_t> d(b.begin(), b.end());
        auto ib = xi::read_image_blob(d, u8pay);
        CHECK(ib.has_value() && ib->width == W, "unknown descriptor keys are skipped, not rejected");
    }
}

// The WRITE flavor over a minted payload (producer mint window).
static void test_write_blob() {
    const int W = 5, H = 3, C = 1;
    std::vector<float> minted((size_t)W * H * C, 0.0f);   // stands in for a mint payload
    cv::Mat m = xi::as_cv_write_blob(minted.data(), W, H, C, "f32");
    CHECK(!m.empty() && m.type() == CV_32FC1, "as_cv_write_blob yields a writable CV_32FC1 Mat");
    CHECK(m.data == (uchar*)minted.data(), "write Mat aliases the minted payload");
    m.setTo(cv::Scalar(2.5));                             // write straight into the payload
    CHECK(minted[0] == 2.5f && minted[W * H - 1] == 2.5f, "writes land in the minted buffer");
    CHECK(xi::as_cv_write_blob(minted.data(), W, H, C, "u4").empty(),
          "as_cv_write_blob rejects an unsupported dtype");
}

int main() {
    std::printf("test_image_blob\n");
    test_dt_matrix();
    test_rejects();
    test_write_blob();
    if (g_fail == 0) { std::printf("  OK (all checks passed)\n"); return 0; }
    std::fprintf(stderr, "  %d check(s) FAILED\n", g_fail);
    return 1;
}
