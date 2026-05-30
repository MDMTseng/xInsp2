//
// status_reporter — publishes a per-instance status string each process()
// via xi::Plugin::status() (host_api->set_status, auto-tagged with the
// instance name). Demonstrates the plugin side of the status channel.
//
#include <xi/xi_plugin_support.hpp>
#include <xi/xi_json.hpp>

class StatusReporter : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& /*input*/) override {
        status("processed " + std::to_string(++n_));   // -> host set_status(name, ...)
        return xi::Record().set("n", n_);
    }

private:
    int n_ = 0;
};

XI_PLUGIN_IMPL(StatusReporter)
