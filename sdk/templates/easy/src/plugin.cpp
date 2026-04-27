//
// {{NAME}} — minimal "easy" project plugin.
//
// This file is auto-generated from a template. Read top-to-bottom — the
// comments walk through every member function in the plugin API.
//
// You don't need to #include anything: xi_plugin_support.hpp is force-
// included by the project-plugin compile path, which pulls in xi_abi.hpp
// (the XI_PLUGIN_IMPL macro), xi_image.hpp (xi::Image), xi_ops.hpp (the
// portable image ops), and xi_record.hpp (xi::Record).
//

#include <string>

class {{CLASS}} {
public:
    // ---- 1. Constructor ----------------------------------------------------
    //
    // Called once when the script (or backend) instantiates this plugin.
    //   host : opaque handle into the BACKEND's image pool. Pass-through
    //          to xi::record_from_c / record_to_c — never read directly.
    //   name : the instance name from project.json (e.g. "filter0").
    //          Useful for log lines and naming sub-resources.
    //
    {{CLASS}}(const xi_host_api* host, const char* name)
        : host_(host), name_(name ? name : "") {}

    // host() accessor is REQUIRED by XI_PLUGIN_IMPL — it's how the macro
    // forwards process() through the host's pool.
    const xi_host_api* host() const { return host_; }

    // ---- 2. get_def / set_def — JSON config -------------------------------
    //
    // get_def() returns a JSON object describing this instance's
    // configuration. The UI panel reads this to render a form, and the
    // project save serializes it. Keep it stable — instance.json on disk
    // round-trips through these two functions.
    //
    std::string get_def() const {
        return "{}";
    }

    bool set_def(const std::string& /*json*/) {
        // Parse 'json' (a JSON object string) and apply to self.
        // Return true on success, false if the JSON was rejected.
        return true;
    }

    // ---- 3. process — the main hook ---------------------------------------
    //
    // Called every time the inspection script does:
    //     auto out = {{NAME}}.process(in_record);
    //
    // 'in' carries images keyed by name and a JSON blob; the returned
    // Record is what the script sees. Empty Record is a valid no-op.
    //
    xi::Record process(const xi::Record& /*in*/) {
        return xi::Record{};
    }

    // ---- 4. exchange — RPC channel ----------------------------------------
    //
    // Lets the script call:
    //     auto rsp = xi::use<{{CLASS}}>("{{NAME}}").exchange("hello");
    // We return a JSON string. Use this for "do X right now" actions that
    // don't fit the per-frame process() flow.
    //
    std::string exchange(const std::string& /*cmd*/) {
        return "{}";
    }

private:
    const xi_host_api* host_;
    std::string        name_;
};

// XI_PLUGIN_IMPL emits the C ABI thunks (xi_plugin_create, _destroy,
// _process, _exchange, _get_def, _set_def) that the backend's loader
// resolves at LoadLibrary time. ALWAYS at file scope, after the class.
XI_PLUGIN_IMPL({{CLASS}})
