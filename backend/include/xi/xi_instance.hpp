#pragma once
//
// xi_instance.hpp — persistent, UI-backed state for xInsp2 inspection
// routines.
//
// An xi::Instance<T> holds a named, type-erased handle to a stateful plugin
// (camera, shape template, classifier, threshold configuration, file
// writer...). It is declared at file scope in the user script, outlives
// individual `inspect()` calls, and is configured through a dedicated UI
// panel driven by the existing plugin UI React components.
//
// Example:
//
//   xi::Instance<ShapeModel>  partTemplate{ "part_template" };
//   xi::Instance<FolderSaver> saver       { "out" };
//
//   void inspect(Image frame) {
//       auto matches = partTemplate->find(frame);
//       saver->write(frame, matches);
//   }
//
// The template parameter T is the plugin class the user wants to work with
// (must derive from xi::InstanceBase). The registry stores type-erased
// InstanceBase pointers so the service can list, configure, and persist
// them uniformly.
//
// This header stays independent of the existing PluginInstanceBase hierarchy
// in xInsp — the adapter that bridges the two lives in the service layer.
//

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace xi {

// Base class for plugin types that can back an xi::Instance<T>.
// Users writing new plugins derive from this. Existing xInsp
// PluginInstanceBase classes are adapted via a thin wrapper in the service.
class InstanceBase {
public:
    virtual ~InstanceBase() = default;

    virtual const std::string& name()        const = 0;
    virtual std::string        plugin_name() const = 0;

    // JSON serialization for project.json persistence.
    virtual std::string get_def() const                 { return "{}"; }
    virtual bool        set_def(const std::string& /*j*/) { return true; }

    // Generic command hook — the frontend sends commands (via WS) that get
    // routed here. Mirrors the existing PluginInstanceBase::exchangeCMD
    // contract without dragging cJSON into this header.
    virtual std::string exchange(const std::string& /*cmd_json*/) { return "{}"; }
};

// Global registry — script declarations populate this at load time.
class InstanceRegistry {
public:
    static InstanceRegistry& instance() {
        static InstanceRegistry r;
        return r;
    }

    // Takes ownership. Duplicate names replace the old entry (the old one is
    // destroyed on replacement — caller must not keep raw pointers).
    void add(std::shared_ptr<InstanceBase> p) {
        std::lock_guard<std::mutex> g(mu_);
        entries_[p->name()] = std::move(p);
    }

    void remove(const std::string& name) {
        std::lock_guard<std::mutex> g(mu_);
        entries_.erase(name);
    }

    std::shared_ptr<InstanceBase> find(const std::string& name) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = entries_.find(name);
        return it == entries_.end() ? nullptr : it->second;
    }

    std::vector<std::shared_ptr<InstanceBase>> list() {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<std::shared_ptr<InstanceBase>> out;
        out.reserve(entries_.size());
        for (auto& [k, v] : entries_) out.push_back(v);
        return out;
    }

    void clear() {
        std::lock_guard<std::mutex> g(mu_);
        entries_.clear();
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<InstanceBase>> entries_;
};

// Plugin factory hook — the service plugin loader calls these to construct
// a plugin by type name. The user script doesn't touch this directly; the
// Instance<T> ctor uses it to obtain a concrete T.
//
// Specialize or provide a non-template overload for each plugin type:
//
//   template <> std::shared_ptr<ShapeModel>
//   make_plugin_instance<ShapeModel>(std::string_view name) { ... }
//
template <class T>
std::shared_ptr<T> make_plugin_instance(std::string_view name);

// User-facing handle. Construction looks up or creates the backing plugin
// and registers it. Copy-constructible — multiple handles pointing at the
// same underlying instance are fine.
template <class T>
class Instance {
    static_assert(std::is_base_of_v<InstanceBase, T>,
                  "xi::Instance<T> requires T : xi::InstanceBase");
public:
    explicit Instance(std::string name) {
        // Reuse an existing instance under this name if present, otherwise
        // ask the factory to build one.
        if (auto existing = InstanceRegistry::instance().find(name)) {
            ptr_ = std::dynamic_pointer_cast<T>(existing);
            if (!ptr_) {
                // Same name is bound to a different type — user error.
                // Leave ptr_ null; first dereference will throw.
                return;
            }
        } else {
            ptr_ = make_plugin_instance<T>(name);
            if (ptr_) InstanceRegistry::instance().add(ptr_);
        }
    }

    T*       operator->()       { return ptr_.get(); }
    const T* operator->() const  { return ptr_.get(); }
    T&       operator*()         { return *ptr_; }
    const T& operator*() const   { return *ptr_; }

    explicit operator bool() const { return static_cast<bool>(ptr_); }

    std::shared_ptr<T> handle() const { return ptr_; }

private:
    std::shared_ptr<T> ptr_;
};

} // namespace xi
