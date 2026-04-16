//
// defect_detection.cpp — example inspection using xi::ops.
//
// Demonstrates the camera trigger model:
// - TestImageSource runs its own acquisition thread inside the DLL
// - cam->grab() dequeues the latest frame
// - The backend calls xi_inspect_entry on each trigger
//

#include <xi/xi.hpp>
#include <xi/xi_image.hpp>
#include <xi/xi_ops.hpp>
#include <xi/xi_source.hpp>

using namespace xi::ops;

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
    // Grab latest frame from the source's internal queue.
    // If the source is running, this returns the newest frame and discards
    // older ones. If the queue is empty (source not started or too slow),
    // grab_wait blocks up to 500ms for a frame.
    xi::Image rgb = cam_source().grab_wait(500);
    if (rgb.empty()) return;  // no frame available

    VAR(input, rgb);

    // Standard inspection pipeline
    VAR(gray,     toGray(input));
    VAR(blurred,  gaussian(gray, blur_radius));
    VAR(binary,   threshold(blurred, thresh_val));
    VAR(cleaned,  dilate(erode(binary, morph_radius), morph_radius));

    // Parallel: edge detection + blob counting
    auto p_edges = async_sobel(gray);
    auto p_blobs = async_countWhiteBlobs(cleaned);

    VAR(edges,      xi::Image(p_edges));
    VAR(blob_count, int(p_blobs));

    // Stats on the grayscale
    auto st = stats(gray);
    VAR(mean_intensity, st.mean);
    VAR(pass, blob_count <= 2);
}
