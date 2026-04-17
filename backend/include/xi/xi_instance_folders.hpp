#pragma once
//
// xi_instance_folders.hpp — name→folder lookup the host_api delegates to.
//
// The PluginManager registers an instance's folder *before* the plugin
// factory runs, so plugins can ask host->instance_folder(name, ...)
// from inside their constructor (or any method) and get a usable path.
//

#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace xi {

class InstanceFolderRegistry {
public:
    static InstanceFolderRegistry& instance() {
        static InstanceFolderRegistry r;
        return r;
    }

    void set(const std::string& name, const std::string& folder) {
        std::unique_lock<std::shared_mutex> lk(mu_);
        map_[name] = folder;
    }

    void clear(const std::string& name) {
        std::unique_lock<std::shared_mutex> lk(mu_);
        map_.erase(name);
    }

    std::string get(const std::string& name) const {
        std::shared_lock<std::shared_mutex> lk(mu_);
        auto it = map_.find(name);
        return it == map_.end() ? std::string{} : it->second;
    }

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::string> map_;
};

} // namespace xi
