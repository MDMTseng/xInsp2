//
// {{NAME}} — minimal "easy" project plugin.
//
// This file is auto-generated from a template. Read top-to-bottom — the
// comments walk through every member function in the plugin API.
//
// You don't need to #include anything: xi_plugin_support.hpp is force-
// included by the project-plugin compile path, which pulls in xi_abi.hpp
// (the XI_PLUGIN_IMPL macro + xi::Plugin base), xi_image.hpp (xi::Image
// + as_cv_mat / create_in_pool helpers), xi_record.hpp, and OpenCV
// (<opencv2/opencv.hpp>). Image ops: call cv:: directly. The base class
// gives you `pool_image(w, h, c)` for zero-copy outputs.
//

#include <string>

class {{CLASS}} : public xi::Plugin {
public:
    // ---- 1. Constructor ----------------------------------------------------
    //
    // Inherit xi::Plugin's ctor — it wires `host_` (opaque pool handle)
    // and `name_` (instance name from project.json) automatically. Add
    // members below the `using` line and initialise them in your own
    // ctor if you need state.
    //
    using xi::Plugin::Plugin;

    // ---- 2. get_def / set_def — JSON config -------------------------------
    //
    // get_def() returns a JSON object describing this instance's
    // configuration. The UI panel reads this to render a form, and the
    // project save serializes it. Keep it stable — instance.json on disk
    // round-trips through these two functions.
    //
    std::string get_def() const override {
        return "{}";
    }

    bool set_def(const std::string& /*json*/) override {
        // Parse 'json' (a JSON object string) and apply to self.
        // Return true on success, false if the JSON was rejected.
        return true;
    }

    // ---- 3. process — the main hook ---------------------------------------
    //
    // Called every time the inspection script does:
    //     auto out = {{NAME}}.process(in_record);
    //
    // Image ops example (replace with your actual logic):
    //
    //   auto src = in.get_image("src");
    //   auto dst = pool_image(src.width, src.height, 1);
    //   cv::GaussianBlur(src.as_cv_mat(), dst.as_cv_mat(), {0, 0}, 2.0);
    //   return xi::Record().image("blurred", dst);
    //
    // `pool_image` (inherited from xi::Plugin) allocates a fresh slot in
    // the host's ImagePool — cv:: writes into it land there directly,
    // and returning the Image short-circuits to addref (no memcpy
    // across the plugin ABI).
    //
    xi::Record process(const xi::Record& /*in*/) override {
        return xi::Record{};
    }

    // ---- 4. exchange — RPC channel ----------------------------------------
    //
    // Lets the script call:
    //     auto rsp = xi::use<{{CLASS}}>("{{NAME}}").exchange("hello");
    // We return a JSON string. Use this for "do X right now" actions that
    // don't fit the per-frame process() flow.
    //
    std::string exchange(const std::string& /*cmd*/) override {
        return "{}";
    }
};

// XI_PLUGIN_IMPL emits the C ABI thunks (xi_plugin_create, _destroy,
// _process, _exchange, _get_def, _set_def) that the backend's loader
// resolves at LoadLibrary time. ALWAYS at file scope, after the class.
XI_PLUGIN_IMPL({{CLASS}})
