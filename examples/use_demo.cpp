//
// use_demo.cpp — demonstrates xi::use() for backend-managed instances.
//
// This is the RECOMMENDED pattern. Instances are created via VS Code UI
// or cmd:create_instance. The script accesses them by name via xi::use().
// Hot-reload does NOT destroy instances or their state.
//

#include <xi/xi.hpp>
#include <xi/xi_ops.hpp>
#include <xi/xi_record.hpp>
#include <xi/xi_use.hpp>

using namespace xi::ops;

xi::Param<int> thresh{"threshold", 128, {0, 255}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    // Access backend-managed camera — survives hot-reload
    auto& cam = xi::use("cam0");
    auto img = cam.grab(500);

    if (img.empty()) {
        // No camera frame — generate a test image
        img = xi::Image(320, 240, 3);
        uint8_t* p = img.data();
        for (int y = 0; y < 240; ++y)
            for (int x = 0; x < 320; ++x) {
                int i = (y * 320 + x) * 3;
                p[i+0] = (uint8_t)((x + frame * 3) & 0xFF);
                p[i+1] = (uint8_t)((y + frame * 5) & 0xFF);
                p[i+2] = (uint8_t)(100);
            }
    }

    VAR(input, img);
    VAR(gray, toGray(img));
    VAR(binary, threshold(gray, (int)thresh));

    int blobs = countWhiteBlobs(binary);
    VAR(blob_count, blobs);
    VAR(pass, blobs <= 5);

    // Persistent state — survives hot-reload
    int count = xi::state()["run_count"].as_int(0);
    xi::state().set("run_count", count + 1);
    xi::state().set("last_blob_count", blobs);

    VAR(run_count, count + 1);
}
