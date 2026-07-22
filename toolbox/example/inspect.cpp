// toolbox integration example — one inspection station, five toolbox plugins.
//
// The per-plugin examples each teach one plugin. This one teaches how they
// COMPOSE, which is the part you cannot learn from any of them individually.
// It is a plausible small station:
//
//     cam   (mock_camera)   grabs the frame
//       |
//     ring  (cache)         retains the sealed pack, so an operator can stop the
//       |                   line and re-inspect the exact frame that failed
//       |
//     det   (blob_analysis) threshold + contours, driven through its pack door
//       |
//     [judge] blob_count against a live-tunable band
//       |
//       +--> view  (expose)      every frame + its numbers, to the UI
//       +--> saver (record_save) ONLY the failures, as canonical .xex1 on disk
//
// The shape worth stealing: the script is the orchestrator. Plugins do not know
// about each other and are not wired to each other — the script pulls from one
// and pushes into the next with xi::use(). Rerouting the station is an edit to
// this file, not a change to any plugin.
//
// Note what rides on the ONE pack plane here: a camera emit, a request/reply
// into blob_analysis, a retention handoff, a file write, and a UI push. Five
// different jobs, one plane, no side channels.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdio>
#include <vector>

// The recipe. All three are live-editable while the line runs — that is the
// point of a Param: retune, replay from `ring`, see the new verdict on the
// frame you are already looking at.
xi::Param<int> threshold{"threshold", 128, xi::Range<int>{0, 255}};
xi::Param<int> min_area {"min_area",    8, xi::Range<int>{1, 10000}};
xi::Param<int> max_blobs{"max_blobs",  64, xi::Range<int>{0, 1000}};

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;

    // ---- 1. the frame, straight off the camera's pack ----------------------
    auto tp = t.pack();
    if (!tp) return;                                   // nothing to judge yet
    const long long seq = (long long)tp.get_i64("seq").value_or(-1);
    auto img = tp.image_blob("frame");
    if (!img || img->payload.empty()) { xi::ng(1, "camera pack carried no frame"); return; }

    // ---- 2. RETAIN before judging ------------------------------------------
    // Cheap — the ring holds a reference to the sealed pack, it does not copy
    // pixels. Do it first so a frame is recoverable even if the judge throws.
    xi::use("ring").process(tp);

    // ---- 3. INSPECT: drive blob_analysis through its pack door -------------
    // Its door is strict on purpose: `gray` must be single-channel u8, and it
    // says so with a fault rather than guessing at a colour conversion it has
    // no business choosing. Converting is the CALLER's job — a station that
    // wants a weighted luma, or only the red channel, gets to say so here.
    const int W = img->width, H = img->height, C = img->channels;
    std::vector<uint8_t> gray((size_t)W * H);
    if (C == 1) {
        gray.assign(img->payload.begin(), img->payload.begin() + (size_t)W * H);
    } else {
        for (size_t i = 0, n = gray.size(); i < n; ++i) {
            unsigned s = 0;
            for (int c = 0; c < C; ++c) s += img->payload[i * (size_t)C + c];
            gray[i] = (uint8_t)(s / (unsigned)C);
        }
    }

    // Request/reply on the pack plane: we build the input, the door answers
    // with a sealed pack. The recipe travels WITH the request, so two scripts
    // can drive the same detector instance with different settings.
    xi::ScriptPackBuilder in;
    in.add_image("gray", W, H, 1, gray.data());
    in.add_i64("threshold", threshold);
    in.add_i64("min_area",  min_area);

    auto out = xi::use("det").process(in.seal());
    if (!out.valid() || out.get_str("$fault")) { xi::ng(2, "blob_analysis door faulted"); return; }
    const long long blobs = (long long)out.get_i64("blob_count").value_or(-1);
    if (blobs < 0) { xi::ng(2, "blob_analysis returned no blob_count"); return; }

    // ---- 4. JUDGE ----------------------------------------------------------
    const bool pass = blobs <= (long long)max_blobs;

    // ---- 5. to the UI, every frame ----------------------------------------
    xi::ScriptPackBuilder v;
    v.add_str("$channel", "station");
    v.add_i64("$seq", (int64_t)xi::run_id());
    v.add_image("frame", img->width, img->height, img->channels, img->payload.data());
    if (auto bin = out.get_image("binary"))
        v.add_image("binary", bin->width, bin->height, bin->channels, bin->pixels.data());
    v.add_i64("blob_count", blobs);
    v.add_i64("threshold", threshold);
    v.add_i64("verdict", pass ? 1 : 0);
    xi::use("view").push(v.seal());

    // ---- 6. to disk, FAILURES ONLY ----------------------------------------
    // Saving every frame is how you fill a disk and never look at any of it.
    // The station keeps only what a human will actually want to re-examine.
    if (!pass) {
        xi::ScriptPackBuilder cap;
        cap.add_str("$channel", "ng");
        cap.add_i64("$seq", seq);
        cap.add_image("frame", img->width, img->height, img->channels, img->payload.data());
        cap.add_i64("blob_count", blobs);
        cap.add_i64("threshold", threshold);
        cap.add_i64("max_blobs", max_blobs);
        auto ack = xi::use("saver").process(cap.seal());
        if (!ack.valid() || ack.get_str("$fault"))
            xi::ng(3, "NG frame could not be persisted");   // losing evidence is
                                                            // its own defect
    }

    char msg[160];
    std::snprintf(msg, sizeof msg, "seq=%lld blobs=%lld (limit %d, thr %d)",
                  seq, blobs, (int)max_blobs, (int)threshold);
    if (pass) xi::ok(1, msg);
    else      xi::ng(1, msg);
}
