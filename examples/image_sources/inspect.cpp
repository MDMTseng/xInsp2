//
// inspect.cpp — demo for the image-source plugins.
//
// Reads the image issued by whichever source kicked this pass (the
// local_image_source thumbnail click, or a cached_image_source replay) via the
// trigger, and surfaces it. No detection logic — this just proves the
// "pick/replay an image -> a pass runs on it" loop.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

#include <string>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    VAR(active, t.is_active());
    if (!t.is_active()) return;

    // The source that issued this pass (its instance name).
    std::string src = t.primary_source();
    VAR(source, src);
    VAR(trigger_id, t.id_string());

    // The input image arrives ON the trigger from whichever source issued this
    // pass — `local` (a file click) or `cache` (a replay). Both emit the frame
    // under key "frame", so one read works either way (a single-image event also
    // resolves by the source name via the host's sole-image fallback).
    auto img = t.image("frame");
    if (img.empty()) { VAR(loaded, false); return; }
    VAR(loaded, true);
    VAR(width, img.width);
    VAR(height, img.height);
    VAR(input, img);     // raw input, surfaced as a preview

    // 1. Capture the input into the cache FIRST (so it can be replayed later) —
    //    but NOT when this pass already IS a cache replay (would re-store it).
    if (src != "cache")
        xi::use("cache").process(xi::Record().image("frame", img));

    // 2. Then binarize. The threshold is a binarize-instance param tuned in its
    //    webui; the new value takes effect on the next pass — replay a cached
    //    frame after moving the slider to see the result update.
    auto bin = xi::use("binarize")
                   .process(xi::Record().image("frame", img))
                   .get_image("binary");
    if (!bin.empty()) VAR(binary, bin);
}
