//
// test_blob_analysis.cpp — developer-side tests for the blob_analysis plugin's
// data contract (docs/new_gen/02-plugin-data-contract.md).
//
// Exercises the three behaviours the contract adds, through the built DLL's C
// ABI + the real host ImagePool (so pool_image and the image getters behave
// exactly as in the backend):
//
//   1. a missing required input ("gray") returns a STRUCTURED failure Record
//      (NA + $fault.code == "missing_input", naming the key + expected type);
//   2. a header/plugin SCHEMA SKEW returns a precise schema_mismatch naming both
//      versions;
//   3. the HAPPY PATH built with xi::blob::Input and read with xi::blob::Output
//      finds the expected blobs.
//
// Build produces blob_analysis_test.exe. Run it; exit code 0 = all pass.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_test.hpp>
#include <xi/xi_record.hpp>
#include <xi/xi_contract.hpp>
#include <xi/xi_json.hpp>

#include "blob_analysis_io.gen.h"

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef BLOB_DLL_PATH
#define BLOB_DLL_PATH "xi-blob_analysis.dll"
#endif

struct Syms {
    xi_plugin_create_fn  create  = nullptr;
    xi_plugin_destroy_fn destroy = nullptr;
    xi_plugin_process_fn process = nullptr;
};
static HMODULE g_dll = nullptr;
static Syms g_syms;
static xi_host_api g_host = xi::ImagePool::make_host_api();

static void load_dll() {
    if (g_dll) return;
    g_dll = LoadLibraryA(BLOB_DLL_PATH);
    if (!g_dll) { std::fprintf(stderr, "failed to load %s (err %lu)\n", BLOB_DLL_PATH, GetLastError()); std::exit(2); }
    g_syms.create  = (xi_plugin_create_fn) GetProcAddress(g_dll, "xi_plugin_create");
    g_syms.destroy = (xi_plugin_destroy_fn)GetProcAddress(g_dll, "xi_plugin_destroy");
    g_syms.process = (xi_plugin_process_fn)GetProcAddress(g_dll, "xi_plugin_process");
    if (!g_syms.create || !g_syms.destroy || !g_syms.process) {
        std::fprintf(stderr, "DLL missing required C ABI exports\n"); std::exit(2);
    }
}

// Create a 1-channel pool image with two filled bright squares (→ 2 blobs).
static xi_image_handle make_gray_two_blobs(int w, int h) {
    xi_image_handle hnd = g_host.image_create(w, h, 1);
    uint8_t* p = g_host.image_data(hnd);
    std::memset(p, 0, (size_t)w * h);
    auto fill = [&](int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) p[y * w + x] = 255;
    };
    fill(2, 2, 8, 8);        // blob A
    fill(20, 20, 30, 30);    // blob B
    return hnd;
}

// Run the plugin's process() once. `in_json` is the input Record JSON; if
// `gray` is non-zero it is passed under key "gray". Returns the output Record
// (reconstructed from the returned JSON) so a test can read it via the extractor.
static xi::Record run_process(const std::string& in_json, xi_image_handle gray) {
    void* inst = g_syms.create(&g_host, "t");
    xi_record_image imgs[1];
    int nimg = 0;
    if (gray) { imgs[0] = { "gray", gray }; nimg = 1; }

    xi_record in{};
    in.images      = nimg ? imgs : nullptr;
    in.image_count = nimg;
    in.data        = reinterpret_cast<const uint8_t*>(in_json.c_str());
    in.len         = static_cast<int32_t>(in_json.size());

    xi_record_out out; xi_record_out_init(&out);
    g_syms.process(inst, &in, &out);

    std::string js;
    if (out.out_doc) {
        size_t n = 0;
        char* s = yyjson_mut_write(reinterpret_cast<yyjson_mut_doc*>(out.out_doc), 0, &n);
        js = s ? std::string(s, n) : "{}";
        if (s) free(s);
    } else {
        js = out.data ? std::string(reinterpret_cast<const char*>(out.data), (size_t)out.len) : "{}";
    }
    xi_record_out_free(&out);
    g_syms.destroy(inst);
    return xi::Record::from_json_bytes(reinterpret_cast<const uint8_t*>(js.data()), js.size());
}

// --- Tests -----------------------------------------------------------------

XI_TEST(blob_missing_gray_is_structured_failure) {
    load_dll();
    // No image at all → the required "gray" input is missing.
    xi::blob::Output out{ run_process("{}", 0) };
    XI_EXPECT(!out.ok());                                  // verdict is fail-loud NA
    XI_EXPECT(out.fault_code() == xi::contract::kMissingInput);
    // The failure names the offending key + expected type.
    xi::Record fault = out.record().get_record(xi::contract::kFaultKey);
    XI_EXPECT(fault.get_string(xi::contract::kKey) == std::string(xi::blob::keys::kGray));
    XI_EXPECT(fault.get_string(xi::contract::kExpectedType) == "image");
}

XI_TEST(blob_schema_skew_names_both_versions) {
    load_dll();
    xi_image_handle gray = make_gray_two_blobs(40, 40);
    // A script built against a future header schema.
    std::string in = xi::Json::object().set(xi::contract::kSchemaKey, 999).dump();
    xi::blob::Output out{ run_process(in, gray) };
    g_host.image_release(gray);

    XI_EXPECT(!out.ok());
    XI_EXPECT(out.fault_code() == xi::contract::kSchemaMismatch);
    xi::Record fault = out.record().get_record(xi::contract::kFaultKey);
    XI_EXPECT(fault.get_int(xi::contract::kHeaderSchema) == 999);
    XI_EXPECT(fault.get_int(xi::contract::kPluginSchema) == xi::blob::kSchemaVersion);
}

XI_TEST(blob_happy_path_via_builder_and_extractor) {
    load_dll();
    xi_image_handle gray = make_gray_two_blobs(40, 40);

    // Build the input with the typed builder (stamps the schema, sets keys).
    std::string in_json = xi::blob::Input().threshold(100).min_area(2).record().data_json();
    xi::blob::Output out{ run_process(in_json, gray) };
    g_host.image_release(gray);

    XI_EXPECT(out.ok());
    XI_EXPECT(out.blob_count() == 2);
    XI_EXPECT(out.blob_count() == out.blobs_size());   // stamped count == array length
    XI_EXPECT(out.threshold_used() == 100);
    // (The "binary" output image rides the Record's image bag, not its JSON;
    // this test reconstructs the output from JSON only, so image round-trip is
    // out of scope here — the contour polygon below, which DOES live in the
    // JSON, is the field the completed extractor makes newly readable.)

    // Read EVERY per-blob output field through the typed extractor — including
    // the contour polygon, which the extractor could NOT read before this task
    // (the io.h exposed only the point count, never the {x,y} data its own .cpp
    // emitted). This is the flagship "leaky veneer" the completion closes.
    for (int b = 0; b < out.blob_count(); ++b) {
        auto blob = out.blob(b);
        XI_EXPECT(blob.area() > 0);
        // Centroid falls inside the bounding box.
        XI_EXPECT(blob.cx() >= blob.min_x() && blob.cx() <= blob.max_x());
        XI_EXPECT(blob.cy() >= blob.min_y() && blob.cy() <= blob.max_y());
        XI_EXPECT(blob.min_x() <= blob.max_x());
        XI_EXPECT(blob.min_y() <= blob.max_y());

        // The stamped point count agrees with the actual emitted polygon length,
        // and every vertex is now reachable AND lands inside the bounding box.
        XI_EXPECT(blob.contour_size() > 0);
        XI_EXPECT(blob.contour_size() == blob.contour_points());
        for (int i = 0; i < blob.contour_size(); ++i) {
            auto pt = blob.contour(i);
            XI_EXPECT(pt.x() >= blob.min_x() && pt.x() <= blob.max_x());
            XI_EXPECT(pt.y() >= blob.min_y() && pt.y() <= blob.max_y());
        }
    }
}

int main() {
    auto results = xi::test::run_all();
    for (auto& r : results) if (!r.passed) return 1;
    return 0;
}
