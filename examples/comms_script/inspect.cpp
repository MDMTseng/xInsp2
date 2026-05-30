//
// inspect.cpp — comms_script. Uses xi::comms to talk to the PLC through the
// out-of-process comms gateway: registers a dead-man, drains PLC-originated
// lines, reports link state, and sends a per-frame result.
//
#include <xi/xi.hpp>
#include <xi/xi_comms.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    xi::comms::set_deadman("{\"cmd\":\"estop\"}");      // PLC emergency if we crash

    auto incoming = xi::comms::poll();                  // PLC -> script (triggers/recipes)
    int n = (int)incoming.size();
    VAR(plc_msgs, n);
    if (n > 0) VAR(first_plc, incoming[0]);

    VAR(plc_up, xi::comms::up() ? 1 : 0);

    xi::comms::send("{\"frame\":" + std::to_string(frame) + ",\"result\":\"PASS\"}");
}
