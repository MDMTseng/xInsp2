//
// inspect.cpp — fe_comms. Identical shape to comms_script, but here the whole
// chain (gateway + backend) is brought up and supervised by xinsp-fe.exe. The
// script just exercises xi::comms every frame so the driver can observe the
// script -> gateway -> PLC path and confirm it survives a gateway respawn.
//
#include <xi/xi.hpp>
#include <xi/xi_comms.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    xi::comms::set_deadman("{\"cmd\":\"estop\"}");      // PLC emergency if we crash

    auto incoming = xi::comms::poll();                  // PLC -> script (if any)
    int n = (int)incoming.size();
    VAR(plc_msgs, n);
    if (n > 0) VAR(first_plc, incoming[0]);

    VAR(plc_up, xi::comms::up() ? 1 : 0);

    xi::comms::send("{\"frame\":" + std::to_string(frame) + ",\"result\":\"PASS\"}");
}
