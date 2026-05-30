//
// inspect.cpp — status_demo. Publishes a sticky status string each run via
// xi::status(). The host keeps the LATEST under "@script", serves it over
// cmd:status (+ a status event), and mirrors it into the crash breadcrumb.
//
#include <xi/xi.hpp>
#include <xi/xi_status.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    xi::status("inspected frame " + std::to_string(frame));
    VAR(frame_no, frame);
}
