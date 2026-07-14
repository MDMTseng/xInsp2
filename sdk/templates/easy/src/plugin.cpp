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
    // Image ops example — read + produce self-describing xi/image BLOBs (spec 30)
    // with the <xi/xi_cv.hpp> sugar (dt-typed cv::Mat views, zero-copy):
    //
    //   auto src = in.image_blob("src");             // {w,h,c,dt,payload}, fail-loud
    //   if (!src) { out.fault("missing_input", "src"); return; }
    //   const int w = src->width, h = src->height;
    //   cv::Mat srcMat = xi::as_cv_read(*src);       // typed by the descriptor's dt
    //   // {"t":"xi/image","w","h","c","dt"} — the convention descriptor.
    //   xi::mp::Writer dw;                            // <xi/xi_mp.hpp>
    //   dw.map(5);
    //   dw.key("t"); dw.str("xi/image"); dw.key("w"); dw.int_(w);
    //   dw.key("h"); dw.int_(h); dw.key("c"); dw.int_(1); dw.key("dt"); dw.str("u8");
    //   void* pp = nullptr;
    //   xi_image_handle bh = out.blob_mint(dw.bytes().data(),
    //                          (int32_t)dw.bytes().size(), (int64_t)w * h, &pp);
    //   if (!bh) { out.fault("no_blob_plane", "binary"); return; }
    //   cv::Mat binMat = xi::as_cv_write_blob(pp, w, h, 1, "u8");  // writable payload
    //   cv::threshold(srcMat, binMat, 128, 255, cv::THRESH_BINARY); // writes IN PLACE
    //   out.adopt_blob("binary", bh);                // pack co-owns (addref)
    //   host_->image_release(bh);                    // drop our mint ref
    //   out.i64("threshold_used", 128);              // scalars: i64/f64/str/boolean
    //
    // Read the INPUT via in.image_blob() (its payload is a zero-copy pool span
    // shared with other consumers — read it, never mutate it) and produce a
    // SEPARATE OUTPUT by minting a headed blob buffer and writing straight into
    // its 64B-aligned payload. NEVER write through the input's payload pointer —
    // that corrupts every other consumer's view of the same pool slot.
    //
    // The zero-copy producer path is blob_mint -> fill payload in place ->
    // adopt_blob (the pack addrefs the pool buffer; you drop your mint ref). The
    // frozen @1 out.image(key, w, h, c, px) / out.adopt_image(...) door still
    // works but COPIES the pixels into a headed xi/image blob (a raw pool buffer
    // has no self-describing head), so an in-tree producer mints the blob itself.
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
