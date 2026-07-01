// throwing_plugin.cpp — B2 regression fixture.
//
// A plugin whose every operator THROWS, built with XI_PLUGIN_IMPL. The macro's
// per-call exports must catch each throw IN THE PLUGIN's OWN RUNTIME and return a
// safe sentinel instead of unwinding across the extern "C" boundary into the host
// (which is UB for a prebuilt / cross-CRT plugin). test_plugin_exception drives
// these exports and asserts the boundary is effectively noexcept.
#include <xi/xi_abi.hpp>

#include <stdexcept>

class ThrowingPlugin : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    xi::Record  process(const xi::Record&)  override { throw std::runtime_error("process boom"); }
    std::string exchange(const std::string&) override { throw std::runtime_error("exchange boom"); }
    std::string get_def() const              override { throw std::runtime_error("get_def boom"); }
    bool        set_def(const std::string&)  override { throw std::runtime_error("set_def boom"); }
};

XI_PLUGIN_IMPL(ThrowingPlugin)
