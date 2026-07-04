//
// hello.cpp — the smallest useful plugin.
//
// Demonstrates: the minimum surface area. No state, no UI, no config.
// The xi.pack@1 door (the sole data plane since the v12 ABI cut) reads one
// string entry from the input pack and answers with a greeting:
//
//   auto out = xi::use("hello0").process(pack);   // pack: str "name"
//   out.get_str("greeting");                      // "hello <name>"
//

#include <xi/xi_abi.hpp>

#include <string>

class Hello : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // Pack in, pack out. in.str() returns std::nullopt when "name" is absent
    // (or not a string entry) — value_or gives the friendly default.
    void process(xi::PackIn& in, xi::PackOut& out) override {
        std::string who(in.str("name").value_or("world"));
        out.str("greeting", "hello " + who);
    }
};

XI_PLUGIN_IMPL(Hello)
// Publish the door: the host probes xi_plugin_get_interface("xi.pack", 1) to
// learn this plugin speaks packs. Always after XI_PLUGIN_IMPL.
XI_PLUGIN_PACK_DOOR(Hello)
