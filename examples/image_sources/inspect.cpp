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

    auto img = t.image(src);
    if (img.empty()) { VAR(loaded, false); return; }
    VAR(loaded, true);
    VAR(width, img.width);
    VAR(height, img.height);
    VAR(preview, img);   // surfaced as an image preview in the UI
}
