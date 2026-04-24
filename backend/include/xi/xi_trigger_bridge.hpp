#pragma once
//
// xi_trigger_bridge.hpp — bridges legacy xi::ImageSource into TriggerBus.
//
// Defines the out-of-line PluginManager::attach_trigger_bridge that
// pairs a freshly-created legacy ImageSource with a publish hook that
// emits its frames to the TriggerBus. New C-ABI plugins call
// host->emit_trigger directly and don't need this bridge.
//
// Include this header from exactly ONE translation unit (service_main.cpp)
// to avoid duplicate-symbol errors at link time.
//

#include "xi_image_pool.hpp"
#include "xi_plugin_manager.hpp"
#include "xi_source.hpp"
#include "xi_trigger_bus.hpp"

namespace xi {

inline void PluginManager::attach_trigger_bridge(InstanceBase* inst,
                                                 const std::string& source_name)
{
    if (!inst) return;
    // Test for ImageSource without dynamic_cast — uses RTTI which is
    // safe for in-process inheritance only when the derived class was
    // compiled against the same xi_source.hpp. With the side-table
    // approach, we register unconditionally; if the instance ISN'T an
    // ImageSource the hook just never fires (push() never gets called).
    if (!dynamic_cast<ImageSource*>(inst)) return;

    ImageSource::register_publish_hook(source_name, [source_name](const Image& img) {
        if (img.empty()) return;
        xi_image_handle h = ImagePool::instance().create(
            img.width, img.height, img.channels);
        std::memcpy(ImagePool::instance().data(h), img.data(), img.size());
        xi_record_image entry { "frame", h };
        TriggerBus::instance().emit(
            source_name, XI_TRIGGER_NULL, /*ts=*/0, &entry, 1);
        ImagePool::instance().release(h);  // bus addref'd internally
    });
}

} // namespace xi
