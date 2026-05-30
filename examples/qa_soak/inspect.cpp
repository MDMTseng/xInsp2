//
// inspect.cpp — qa_soak. A deliberately boring, healthy script: every frame it
// pushes one line to the PLC (through the comms gateway) and reports the frame
// number. No crashes, no heavy work — the point is sustained NORMAL operation so
// the soak driver can assert the supervisor never trips a false safe-state and
// the heartbeat keeps advancing.
//
#include <xi/xi.hpp>
#include <xi/xi_comms.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    xi::comms::send("{\"frame\":" + std::to_string(frame) + ",\"ok\":true}");
    VAR(frame_no, frame);
    VAR(plc_up, xi::comms::up() ? 1 : 0);
}
