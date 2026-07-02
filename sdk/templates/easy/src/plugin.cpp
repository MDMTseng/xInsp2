//
// {{NAME}} — "easy" template: the base skeleton (Layer 0 — process() only).
//
// The three project-plugin templates are ONE skeleton with progressively
// more layers turned on — same base class, same helpers, never a different
// architecture:
//
//   easy    Layer 0  process() only                         ← you are here
//   medium  Layer 1  + config/params (xi::Json) + status()  → an image op
//   expert  Layer 2  + a source worker (xi::spawn_worker)    → pushes frames
//                       emitting via the blessed emit() path
//
// Every tier is `public xi::Plugin`, so every tier gets pool_image(),
// status(), compress(), emit(), and the capability wrappers for free. Pick a
// tier by how much you need, not by a different style.
//
// You don't need to #include anything for the basics: xi_plugin_support.hpp
// is force-included by the project-plugin compile path, which pulls in
// xi_abi.hpp (the XI_PLUGIN_IMPL macro + xi::Plugin base), xi_image.hpp
// (xi::Image + xi::Image::create_in_pool), xi_record.hpp, and xi_cv.hpp
// (OpenCV + the xi::as_cv_read / xi::as_cv_write zero-copy bridges). Image
// ops: call cv:: directly.
//

class {{CLASS}} : public xi::Plugin {
public:
    // ---- Constructor -------------------------------------------------------
    //
    // Inherit xi::Plugin's ctor — it wires `host_` (opaque pool handle) and
    // `name_` (instance name from project.json) automatically. Add members
    // below the `using` line and initialise them in your own ctor if you need
    // state (the medium/expert tiers do exactly that).
    //
    using xi::Plugin::Plugin;

    // ---- process — the one hook this tier overrides ------------------------
    //
    // Called every time the inspection script does:
    //     auto out = xi::use("{{NAME}}").process(in_record);
    //
    // Image ops example (replace with your actual logic):
    //
    //   auto src = in.get_image("src");                  // read-only INPUT
    //   auto dst = pool_image(src.width, src.height, 1);  // writable OUTPUT
    //   cv::threshold(xi::as_cv_read(src), xi::as_cv_write(dst), 128, 255,
    //                 cv::THRESH_BINARY);
    //   return xi::Record().image("binary", dst);
    //
    // Read the INPUT via as_cv_read (an input is a zero-copy view shared with
    // other consumers — read it, never mutate it) and produce a SEPARATE OUTPUT
    // via pool_image + as_cv_write. NEVER do an in-place cv:: op on an input
    // (e.g. cv::threshold(as_cv_read(src), as_cv_read(src), ...)) — that corrupts
    // every other consumer's view of the same pool slot.
    //
    // `pool_image` (inherited from xi::Plugin) allocates a fresh slot in the
    // host's ImagePool — cv:: writes into it land there directly, and returning
    // the Image short-circuits to addref (no memcpy across the plugin ABI).
    //
    xi::Record process(const xi::Record& /*in*/) override {
        return xi::Record{};
    }

    // get_def()/set_def() (JSON config), exchange() (the script/UI RPC
    // channel), status(), and the frame-perfect config swap are all inherited
    // from xi::Plugin with safe defaults — this tier doesn't need them. The
    // medium template turns on config + exchange + status; the expert template
    // adds a background source worker on top. Same base, one more layer.
};

// XI_PLUGIN_IMPL emits the C ABI thunks (xi_plugin_create, _destroy,
// _process, _exchange, _get_def, _set_def) that the backend's loader
// resolves at LoadLibrary time. ALWAYS at file scope, after the class.
XI_PLUGIN_IMPL({{CLASS}})
