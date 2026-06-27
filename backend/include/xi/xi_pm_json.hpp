#pragma once
//
// xi_pm_json.hpp — minimal JSON-output helpers shared by the plugin/project
// machinery. The escape primitive now lives in xi_json_escape.hpp (one copy for
// the whole backend); these names stay as thin forwarders so the many
// pm_json_escape / pm_json_quote call sites in xi_plugin_manager.hpp are
// unchanged.
//
// Header-only, std lib only — no platform coupling.
//
#include <string>

#include "xi_json_escape.hpp"

namespace xi {

// Append a JSON-quoted, escaped form of `s` to `out`. Forwards to the one
// escape primitive (xi_json_escape.hpp) so project.json / plugin metadata agree
// with the rest of the backend on encoding. (Backspace/formfeed now emit as the
// six-char unicode-escape form rather than the short two-char escape — both are
// valid and re-parse identically; such bytes don't occur in plugin/instance/
// group names or paths anyway.)
inline void pm_json_escape(std::string& out, const std::string& s) {
    json_escape_into(out, s);
}

// Convenience: returns a fresh quoted+escaped string.
inline std::string pm_json_quote(const std::string& s) {
    std::string out;
    pm_json_escape(out, s);
    return out;
}

} // namespace xi
