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
// status(), compress(), new_pack()/emit(), and the capability wrappers for
// free. Pick a tier by how much you need, not by a different style.
//
// You don't need to #include anything for the basics: xi_plugin_support.hpp
// is force-included by the project-plugin compile path, which pulls in
// xi_abi.hpp (the XI_PLUGIN_IMPL / XI_PLUGIN_PACK_DOOR macros, the xi::Plugin
// base, and the xi::PackIn / xi::PackOut views over the xi.pack@1 data
// plane), xi_image.hpp (xi::Image + the host ImagePool bridge), and xi_cv.hpp
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
    // The xi.pack@1 pack-in/pack-out door — THE data plane (the old
    // xi::Record process() path was deleted in the v12 ABI cut). Called every
    // time the inspection script does:
    //     auto out = xi::use("{{NAME}}").process(in_pack);
    //
    // Image ops example (replace with your actual logic):
    //
    //   auto src = in.image("src");                  // read-only INPUT view
    //   if (!src) { out.fault("missing_input", "src"); return; }
    //   xi::Image dst = pool_image(src->width, src->height, 1);  // writable OUTPUT
    //   xi::Image srcView = xi::Image::view(src->width, src->height,
    //                                       src->channels,
    //                                       static_cast<const uint8_t*>(src->pixels));
    //   cv::threshold(xi::as_cv_read(srcView), xi::as_cv_write(dst), 128, 255,
    //                 cv::THRESH_BINARY);
    //   out.adopt_image("binary", dst.width, dst.height, dst.channels,
    //                   dst.pool_handle());          // zero-copy handoff
    //   out.i64("threshold_used", 128);              // scalars: i64/f64/str/boolean
    //
    // Read the INPUT via in.image() (its pixels are a zero-copy pool span
    // shared with other consumers — read it, never mutate it) and produce a
    // SEPARATE OUTPUT via pool_image + as_cv_write. NEVER write through the
    // input's pixel pointer — that corrupts every other consumer's view of
    // the same pool slot. The discipline is enforced: an input view is not
    // writable, so as_cv_write on it yields an empty Mat instead of silent
    // corruption.
    //
    // `pool_image` (inherited from xi::Plugin) allocates a fresh slot in the
    // host's ImagePool — cv:: writes into it land there directly, and
    // out.adopt_image(..., dst.pool_handle()) hands the slot to the pack by
    // refcount (no memcpy across the plugin ABI). out.image(key, w, h, c, px)
    // is the copying variant for pixels that don't live in the pool.
    //
    // Leaving `out` untouched seals an EMPTY pack — the door's "no output"
    // sentinel. A contract failure is a NORMAL sealed pack stamped with
    // out.fault(code, key, detail) — fail loud, never a silent default.
    //
    void process(xi::PackIn& /*in*/, xi::PackOut& /*out*/) override {
    }

    // get_def()/set_def() (JSON config), exchange() (the script/UI RPC
    // channel), status(), and the frame-perfect config swap are all inherited
    // from xi::Plugin with safe defaults — this tier doesn't need them. The
    // medium template turns on config + exchange + status; the expert template
    // adds a background source worker on top. Same base, one more layer.
};

// XI_PLUGIN_IMPL emits the C ABI thunks (xi_plugin_create, _destroy,
// _exchange, _get_def, _set_def, _abi_version) that the backend's loader
// resolves at LoadLibrary time. THE CUT (v12): there is no _process thunk —
// the data plane is the xi.pack@1 door, published separately by
// XI_PLUGIN_PACK_DOOR (it exports xi_plugin_get_interface, which the host
// probes to learn this plugin speaks packs). Both ALWAYS at file scope,
// after the class, in this order.
XI_PLUGIN_IMPL({{CLASS}})
XI_PLUGIN_PACK_DOOR({{CLASS}})
