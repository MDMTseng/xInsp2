// Phase C-2 smoke: register an emergency PLC payload (host->set_safe_state) and
// then crash uncatchably (raw thread null-deref). The BE persists the payload to
// <project>/.xinsp_safestate before dying; the supervising FE reads it and
// forwards it to the PLC — proving the "BE crashed -> tell the PLC THIS"
// guarantee survives without resident comms in core.
#include <xi/xi.hpp>
#include <thread>
class Crasher : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    xi::Record process(const xi::Record& /*in*/) override {
        if (host() && host()->set_safe_state)
            host()->set_safe_state("{\"cmd\":\"estop\",\"by\":\"plugin\"}");
        std::thread t([] { *(volatile int*)nullptr = 0xDEAD; });
        t.join();   // never returns — tears the process down
        return xi::Record().set("count", 1);
    }
};
XI_PLUGIN_IMPL(Crasher)
