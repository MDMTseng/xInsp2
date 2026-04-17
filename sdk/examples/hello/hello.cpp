//
// hello.cpp — the smallest useful plugin.
//
// Demonstrates: the minimum surface area. No state, no UI, no config.
// process() reads one field from input and returns a greeting.
//

#include <xi/xi_abi.hpp>

class Hello : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        std::string who = input["name"].as_string("world");
        return xi::Record().set("greeting", "hello " + who);
    }
};

XI_PLUGIN_IMPL(Hello)
