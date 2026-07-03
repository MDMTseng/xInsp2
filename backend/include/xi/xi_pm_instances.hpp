#pragma once
//
// xi_pm_instances.hpp — PluginManager instance CRUD, lifecycle state, and
// project/instance JSON persistence.
//
// Out-of-class inline definitions for:
//   is_valid_instance_name / create_instance / save_instance / remove_instance /
//   rename_instance / instance_group / set_instance_state / get_instance_state /
//   note_instance_crash / clear_instance_states / to_json / open_warnings /
//   merge_unknown_top_keys_ / save_project_locked / save_instance_json
//
// Mechanically extracted from xi_plugin_manager.hpp — bodies are byte-identical
// to the former in-class definitions.
//
#include "xi_pm_manager_core.hpp"
#include "xi_project.hpp"   // kProjectSchema — the project-file schema identity

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace xi {

// An instance name becomes a folder under <project>/instances/, so it MUST be
// a single safe path segment — never a path. Without this an absolute path or
// a `..` in the name escapes the project folder (create writes / remove
// recursively deletes / rename moves an attacker- or typo-chosen directory).
// Allow only identifier-ish chars; reject separators, drive colon, and `..`.
inline bool PluginManager::is_valid_instance_name(const std::string& n) {
    if (n.empty() || n.size() > 128) return false;
    if (n == "." || n.find("..") != std::string::npos) return false;
    for (unsigned char c : n)
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
    return true;
}

// Create a new instance of a plugin inside the current project.
inline InstanceInfo* PluginManager::create_instance(const std::string& instance_name,
                               const std::string& plugin_name,
                               std::string* err) {
    auto fail = [&](std::string msg) -> InstanceInfo* { if (err) *err = std::move(msg); return nullptr; };
    std::lock_guard<std::mutex> lk(mu_);
    if (project_.folder_path.empty())
        return fail("no project open — open_project before creating an instance");
    if (!is_valid_instance_name(instance_name))
        return fail("invalid instance name '" + instance_name +
                    "' — use letters/digits/_/-/. only (no path separators or '..')");

    auto pit = plugins_.find(plugin_name);
    if (pit == plugins_.end())
        return fail("plugin '" + plugin_name + "' not loaded");
    auto& pi = pit->second;
    if (!pi.c_factory)
        return fail("plugin '" + plugin_name + "' has no factory (load_plugin failed?)");

    auto inst_folder = std::filesystem::path(project_.folder_path) / "instances" / instance_name;
    // Use the error_code overload: the throwing one would propagate a
    // filesystem_error past the (un-try/catch'd) WS command loop and
    // terminate the whole backend on a benign environment failure (path too
    // long, instances/ read-only, disk full). Report it as a command error.
    {
        std::error_code ec;
        std::filesystem::create_directories(inst_folder, ec);
        if (ec)
            return fail("cannot create instance folder '" + inst_folder.string() +
                        "': " + ec.message());
    }

    InstanceInfo ii;
    ii.name = instance_name;
    ii.plugin_name = plugin_name;
    ii.folder_path = inst_folder.string();

    // Register the folder BEFORE the factory runs so the plugin can
    // call host->instance_folder() from inside its constructor.
    InstanceFolderRegistry::instance().set(instance_name, ii.folder_path);

    // V3 precedence: if this plugin is machine-autoloaded, evict the machine
    // provider FIRST so this explicit project instance registers its capability
    // names cleanly (no ETAKEN double-register). The machine provider is
    // reinstated when the project instance is removed (remove_instance) or the
    // project closes (close_project).
    evict_machine_provider_locked_(plugin_name);

    ImagePoolOwnerId created_owner = 0;   // for a later sweep if a save fails
    {
        // C ABI — create via host API. ImagePoolOwnerScope tags every image the
        // ctor allocates and AUTO-SWEEPS them on any early return below (throw /
        // null) — the leak-on-failed-ctor footgun is now structural, not manual.
        xi_host_api& host = default_host_api();
        ImagePoolOwnerScope owner;
        created_owner = owner.id();   // for the post-adopt save-failure sweep
        void* raw = nullptr;
        try {
            raw = owner.run_factory([&] { return pi.c_factory(&host, instance_name.c_str()); });
        } catch (const std::exception& e) {
            // A throwing ctor must not terminate the backend (the WS command
            // loop has no outer catch). owner sweeps the half-init handles; we
            // still clear the folder registry and report it as a command error.
            InstanceFolderRegistry::instance().clear(instance_name);
            return fail("plugin '" + plugin_name + "' factory threw: " + e.what());
        } catch (...) {
            InstanceFolderRegistry::instance().clear(instance_name);
            return fail("plugin '" + plugin_name + "' factory threw a non-standard exception");
        }
        if (!raw) {
            InstanceFolderRegistry::instance().clear(instance_name);
            return fail("plugin '" + plugin_name + "' factory returned null "
                        "(constructor failed/rejected the config)");
        }
        auto adapter = std::make_shared<CAbiInstanceAdapter>(
            instance_name, plugin_name, pi.handle, raw, pi.reentrant, /*max_concurrency=*/0, pi.is_sink);
        adapter->adopt_owner_id(owner.release());   // adapter owns the sweep now
        // item 14: a fresh instance takes the plugin's default policy (no
        // instance.json override yet); arm the in-place reinit for on_fault=reinit.
        ii.on_fault = pi.default_on_fault;
        adapter->set_on_fault(ii.on_fault);
        adapter->arm_reinit(pi.c_factory, &host);
        ii.instance = std::move(adapter);
    }
    InstanceRegistry::instance().add(ii.instance);

    // Save instance.json. If the write fails (disk full / read-only) don't
    // commit the instance: leaving it only in project.json (not on disk) would
    // make the next open_project silently lose the user's config. Sweep the
    // registries + folder and report the failure. (commit_working_copy applies
    // the same check on its pre-commit saves.)
    if (!save_instance_json(ii)) {
        InstanceRegistry::instance().remove(instance_name);
        InstanceFolderRegistry::instance().clear(instance_name);
        ImagePool::instance().release_all_for(created_owner);
        return fail("cannot write instance config for '" + instance_name +
                    "' (disk full / read-only?) — instance not created");
    }

    project_.instances[instance_name] = std::move(ii);
    set_state_locked_(instance_name, InstState::Created, "");   // born Created
    save_project_locked();
    return &project_.instances[instance_name];
}

// Save an instance's current config to its folder.
// Returns false if the instance is unknown OR its on-disk write failed.
inline bool PluginManager::save_instance(const std::string& instance_name) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = project_.instances.find(instance_name);
    if (it == project_.instances.end()) return false;
    return save_instance_json(it->second);
}

// Remove an instance: destroys the runtime object + unregisters from
// both registries. Optionally deletes the on-disk folder.
inline bool PluginManager::remove_instance(const std::string& instance_name, bool delete_folder) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = project_.instances.find(instance_name);
    if (it == project_.instances.end()) return false;
    InstanceRegistry::instance().remove(instance_name);
    InstanceFolderRegistry::instance().clear(instance_name);
    inst_state_.erase(instance_name);   // lifecycle state dies with the instance
    std::string folder = it->second.folder_path;
    project_.instances.erase(it);
    if (delete_folder && !folder.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
    } else if (!folder.empty()) {
        // "keep folder" must still PERSIST the removal. open_project discovers
        // instances by scanning instances/<name>/instance.json (the project.json
        // list is write-only / ignored on load), so leaving instance.json in
        // place would resurrect this instance on the next open. Move it aside to
        // instance.json.removed: the scan skips a folder with no instance.json,
        // yet the config + any assets stay on disk for manual recovery, which is
        // what "keep folder" means. (A later create_instance of the same name
        // writes a fresh instance.json, shadowing the tombstone.)
        std::error_code ec;
        auto ij = std::filesystem::path(folder) / "instance.json";
        if (std::filesystem::exists(ij, ec))
            std::filesystem::rename(ij, std::filesystem::path(folder) / "instance.json.removed", ec);
    }
    save_project_locked();
    // V3: if that was the last project instance of an autoload lib plugin, the
    // machine provider steps back in (idempotent — no-op for non-autoload plugins
    // and for plugins still project-provided).
    autoload_machine_providers_locked_();
    return true;
}

// Rename an instance. Moves the on-disk folder and re-registers under the new
// name. Rejected if the new name is invalid / in use / the instance is missing
// (no side effects); Ok on full success; NotPersisted if the runtime renamed
// but the config write failed.
inline PluginManager::RenameResult PluginManager::rename_instance(const std::string& old_name, const std::string& new_name) {
    std::lock_guard<std::mutex> lk(mu_);
    if (old_name == new_name) return RenameResult::Ok;
    if (!is_valid_instance_name(new_name)) return RenameResult::Rejected;   // no path escape via the name
    auto it = project_.instances.find(old_name);
    if (it == project_.instances.end()) return RenameResult::Rejected;
    if (project_.instances.count(new_name)) return RenameResult::Rejected;

    // Validate everything that can fail WITHOUT side effects BEFORE touching
    // the filesystem / registries — a failure here used to leave the folder
    // already renamed with no rollback, orphaning the old instance.
    auto pit = plugins_.find(it->second.plugin_name);
    if (pit == plugins_.end()) return RenameResult::Rejected;
    auto& pi = pit->second;
    if (!pi.c_factory) return RenameResult::Rejected;

    auto old_folder = std::filesystem::path(it->second.folder_path);
    auto new_folder = old_folder.parent_path() / new_name;
    if (std::filesystem::exists(new_folder)) return RenameResult::Rejected;

    // Capture what we need from the old entry before any mutation.
    const std::string plugin_name      = it->second.plugin_name;
    const int         old_max_conc      = it->second.max_concurrency;
    const std::string old_group         = it->second.group;
    const OnFault     old_on_fault      = it->second.on_fault;   // item 14
    std::string saved_def;
    if (it->second.instance) saved_def = it->second.instance->get_def();

    // Move the folder. From here, any failure must roll this back so the OLD
    // instance (still fully registered, untouched below until success) keeps
    // working.
    std::error_code ec;
    std::filesystem::rename(old_folder, new_folder, ec);
    if (ec) return RenameResult::Rejected;
    auto rollback_folder = [&]() {
        std::error_code re; std::filesystem::rename(new_folder, old_folder, re);
    };

    // InstanceBase::name() is immutable, so recreate under the new name via
    // the factory; the old instance's config is carried via get_def→set_def.
    // Register the new folder BEFORE the factory so the ctor can read it.
    InstanceFolderRegistry::instance().set(new_name, new_folder.string());
    xi_host_api& host = default_host_api();
    // ImagePoolOwnerScope tags + auto-sweeps the ctor's images so a rename can't
    // orphan them at owner 0 (the leak this guards against). Mirrors
    // create_instance / project-load via the one RAII primitive.
    ImagePoolOwnerScope owner;
    void* raw = nullptr;
    try {
        raw = owner.run_factory([&] { return pi.c_factory(&host, new_name.c_str()); });
    } catch (...) {
        // A throwing ctor must not terminate the backend (no outer catch on
        // the WS loop). owner sweeps; undo the new-name registration + folder
        // move so the old instance stays intact.
        InstanceFolderRegistry::instance().clear(new_name);
        rollback_folder();
        return RenameResult::Rejected;
    }
    if (!raw) {
        InstanceFolderRegistry::instance().clear(new_name);
        rollback_folder();
        return RenameResult::Rejected;
    }

    // Factory succeeded — now commit: drop the old runtime entries and swap in
    // the new instance.
    InstanceRegistry::instance().remove(old_name);
    InstanceFolderRegistry::instance().clear(old_name);

    InstanceInfo ii;
    ii.name = new_name;
    ii.plugin_name = plugin_name;
    ii.folder_path = new_folder.string();
    // F2: carry over per-instance config that isn't in get_def() — else a
    // rename silently dropped the concurrency cap + dispatch group.
    ii.max_concurrency = old_max_conc;
    ii.group           = old_group;
    ii.on_fault        = old_on_fault;   // item 14: rename preserves the policy
    auto adapter = std::make_shared<CAbiInstanceAdapter>(
        new_name, plugin_name, pi.handle, raw, pi.reentrant, ii.max_concurrency, pi.is_sink);
    adapter->adopt_owner_id(owner.release());   // ctor images now belong to the live adapter
    adapter->set_on_fault(ii.on_fault);
    adapter->arm_reinit(pi.c_factory, &host);
    ii.instance = std::move(adapter);
    if (!saved_def.empty()) ii.instance->set_def(saved_def);
    InstanceRegistry::instance().add(ii.instance);

    project_.instances.erase(old_name);
    project_.instances[new_name] = std::move(ii);
    // Carry the host-tracked lifecycle state across the rename in the SAME
    // locked op as the registries (old_name != new_name is guaranteed above).
    // This is why the WS handler no longer needs a separate rename_inst_state
    // call — the state can't drift from the instance set anymore.
    if (auto sit = inst_state_.find(old_name); sit != inst_state_.end()) {
        inst_state_[new_name] = sit->second;
        inst_state_.erase(sit);
    }
    // Surface a write failure: the runtime + folder are already renamed, so if
    // project.json/instance.json don't persist the new name the next open is
    // inconsistent. Return NotPersisted (NOT Rejected) so the caller still
    // reports it as a save failure rather than a rename failure (the rename
    // itself happened).
    bool saved = save_instance_json(project_.instances[new_name]);
    saved = save_project_locked() && saved;
    return saved ? RenameResult::Ok : RenameResult::NotPersisted;
}

// Thread-safe dispatch-group lookup for the bus sink, which runs on a source
// plugin's emit thread concurrently with create/remove/rename_instance (those
// mutate project_.instances under mu_). Reading project().instances unlocked
// from the sink was a data race (find() vs erase() → UAF). Returns the
// instance's group, or default_group when absent/unknown.
inline std::string PluginManager::instance_group(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!name.empty()) {
        auto it = project_.instances.find(name);
        if (it != project_.instances.end() && !it->second.group.empty())
            return it->second.group;
    }
    return project_.default_group;
}

inline void PluginManager::set_instance_state(const std::string& name, InstState s,
                        const std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    set_state_locked_(name, s, err);
}
// Returns true if a state has been recorded for `name` (filling out_*); false
// if unknown (the caller may then fall back to "exists ⇒ Created").
inline bool PluginManager::get_instance_state(const std::string& name, InstState& out_state,
                        std::string& out_err, long long* out_crash_count) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = inst_state_.find(name);
    if (it == inst_state_.end()) return false;
    out_state = it->second.state;
    out_err   = it->second.last_error;
    if (out_crash_count) *out_crash_count = it->second.crash_count;
    return true;
}
// Record a process() fault for `name` (called from a dispatch worker on an SEH/
// exception crash — exceptional, so taking mu_ here is fine). Bumps the count and
// notes the cause; the instance stays its current state (the config is fine, the
// run crashed) but the count makes a crash loop visible via get_state.
inline void PluginManager::note_instance_crash(const std::string& name, const std::string& why) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& r = inst_state_[name];
    ++r.crash_count;
    r.last_error = why;
}
inline void PluginManager::clear_instance_states() {
    std::lock_guard<std::mutex> lk(mu_);
    inst_state_.clear();
}

inline std::string PluginManager::to_json() {
    std::lock_guard<std::mutex> lk(mu_);
    std::string out = "{\"name\":";
    pm_json_escape(out, project_.name);
    out += ",\"script\":";
    pm_json_escape(out, std::filesystem::path(project_.script_path).filename().string());
    out += ",\"instances\":[";
    int i = 0;
    for (auto& [k, v] : project_.instances) {
        if (i++) out += ",";
        out += "{\"name\":";  pm_json_escape(out, v.name);
        out += ",\"plugin\":"; pm_json_escape(out, v.plugin_name);
        out += "}";
    }
    out += "],\"plugins\":[";
    i = 0;
    for (auto& [k, v] : plugins_) {
        if (i++) out += ",";
        bool is_proj = project_plugin_origin_.count(v.name) > 0;
        out += "{\"name\":"; pm_json_escape(out, v.name);
        out += ",\"description\":"; pm_json_escape(out, v.description);
        out += ",\"has_ui\":" + std::string(v.has_ui ? "true" : "false");
        out += ",\"loaded\":" + std::string(v.handle ? "true" : "false");
        out += ",\"prebuilt\":" + std::string(v.prebuilt ? "true" : "false");
        // origin: "project" if compiled from <project>/plugins, else "global"
        out += ",\"origin\":\"" + std::string(is_proj ? "project" : "global") + "\"";
        if (is_proj) {
            out += ",\"source_dir\":";
            pm_json_escape(out, project_plugin_origin_[v.name]);
        }
        // Optional `manifest` block from plugin.json — passed through
        // verbatim. Clients (AI agents, doc tools) parse the body
        // themselves; the backend doesn't validate or reshape it.
        if (!v.manifest_json.empty()) {
            out += ",\"manifest\":" + v.manifest_json;
        }
        out += "}";
    }
    out += "]";
    // Working-copy status so the UI can target the scratch dir for editing
    // and surface a "save project" affordance.
    out += ",\"working_copy\":" + std::string(canonical_path_.empty() ? "false" : "true");
    if (!canonical_path_.empty()) {
        out += ",\"canonical_path\":"; pm_json_escape(out, canonical_path_);
        out += ",\"working_dir\":";    pm_json_escape(out, project_.folder_path);
    }
    out += "}";
    return out;
}

inline std::vector<OpenWarning> PluginManager::open_warnings() {
    std::lock_guard<std::mutex> lk(mu_);
    return last_open_warnings_;
}

inline std::string PluginManager::open_error() {
    std::lock_guard<std::mutex> lk(mu_);
    return last_open_error_;
}

// Carry over any top-level project.json keys this writer doesn't manage
// (e.g. `params`, and fields another tool/the VS Code extension adds like
// `auto_respawn` / `watchdog_ms`). save_project_locked is a FULL rebuild, so
// without this it silently DROPS them on every instance CRUD — the same
// data-loss class as the F1 groups/runtime fix, but for fields a *different*
// writer owns. Merge-not-clobber: the freshly-built JSON wins for keys it
// emits; every other top-level key from the existing file survives verbatim.
inline std::string PluginManager::merge_unknown_top_keys_(const std::string& fresh_json,
                                           const std::filesystem::path& existing) {
    std::error_code ec;
    if (!std::filesystem::exists(existing, ec)) return fresh_json;
    std::ifstream f(existing, std::ios::binary);
    std::string old((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (old.empty()) return fresh_json;
    yyjson_doc* od = yyjson_read(old.data(), old.size(), 0);
    yyjson_doc* nd = yyjson_read(fresh_json.data(), fresh_json.size(), 0);
    std::string result = fresh_json;
    if (od && nd) {
        yyjson_val* oroot = yyjson_doc_get_root(od);
        yyjson_val* nroot = yyjson_doc_get_root(nd);
        if (yyjson_is_obj(oroot) && yyjson_is_obj(nroot)) {
            yyjson_mut_doc* md = yyjson_doc_mut_copy(nd, nullptr);
            yyjson_mut_val* mroot = md ? yyjson_mut_doc_get_root(md) : nullptr;
            if (mroot) {
                // The top-level keys THIS writer manages. Must be an EXPLICIT
                // allowlist, not "does the key appear in the fresh JSON": some
                // managed keys (runtime / plugin_dirs / plugins) are emitted
                // CONDITIONALLY (omitted when empty). Inferring ownership from
                // presence would treat an intentionally-emptied managed key as
                // "unknown" and RESURRECT its old on-disk value — clearing would
                // never persist. Anything NOT in this set is owned by another
                // writer (params / auto_respawn / watchdog_ms / future) → carry over.
                static const char* const kOwned[] = {
                    "schema", "name", "script", "parallelism", "runtime",
                    "instances", "plugin_dirs", "plugins",
                };
                size_t idx, mx; yyjson_val *k, *v;
                yyjson_obj_foreach(oroot, idx, mx, k, v) {
                    const char* key = yyjson_get_str(k);
                    if (!key) continue;
                    bool owned = false;
                    for (const char* o : kOwned) if (std::strcmp(key, o) == 0) { owned = true; break; }
                    if (owned) continue;                      // managed (even when omitted) → don't resurrect
                    if (yyjson_obj_get(nroot, key)) continue; // also present in fresh → new wins
                    yyjson_mut_val* mk = yyjson_mut_strcpy(md, key);
                    yyjson_mut_val* mv = yyjson_val_mut_copy(md, v);
                    if (mk && mv) yyjson_mut_obj_add(mroot, mk, mv);
                }
                if (char* w = yyjson_mut_write(md, YYJSON_WRITE_PRETTY, nullptr)) {
                    result.assign(w);
                    free(w);
                }
            }
            if (md) yyjson_mut_doc_free(md);
        }
    }
    if (od) yyjson_doc_free(od);
    if (nd) yyjson_doc_free(nd);
    return result;
}

inline bool PluginManager::save_project_locked() {
    auto pj = std::filesystem::path(project_.folder_path) / "project.json";
    // BUG#4 quarantine guard: if project.json was non-empty-but-unparseable
    // at open, a full rebuild here would (a) drop extension-owned top-level
    // keys (params / auto_respawn / watchdog_ms) — merge_unknown_top_keys_
    // can't carry them over because it re-reads the SAME corrupt file and
    // yyjson_read returns null — and (b) overwrite the only recoverable copy
    // of the user's bytes. Refuse the destructive write and keep the project
    // degraded until a fresh open of a valid file clears the flag. The
    // original bytes were already preserved as project.json.corrupt-<ts> at
    // open. This mirrors the compile-fail path: stay up, don't clobber.
    if (project_json_malformed_) {
        std::fprintf(stderr,
            "[xinsp2] save_project_locked: refusing to overwrite malformed "
            "project.json for %s — fix the file and reopen (original bytes "
            "preserved as project.json.corrupt-<ts>).\n",
            pj.string().c_str());
        return false;
    }
    std::string out = "{\n";
    // Schema identity (Finding 2 / adoption item 12): stamp the project-file
    // format so a future reader can recognize (and, at a breaking bump, refuse)
    // the format rather than guess. Same family/naming as the run-outcome schema.
    out += "  \"schema\": \""; out += xi::project::kProjectSchema; out += "\",\n";
    out += "  \"name\": "; pm_json_escape(out, project_.name); out += ",\n";
    out += "  \"script\": ";
    pm_json_escape(out, std::filesystem::path(project_.script_path).filename().string());
    out += ",\n";
    out += "  \"parallelism\": {";
    out += "\"dispatch_threads\":" + std::to_string(project_.dispatch_threads);
    out += ",\"queue_depth\":"     + std::to_string(project_.queue_depth);
    out += ",\"max_inflight\":"    + std::to_string(project_.max_inflight);
    out += ",\"overflow\":";
    pm_json_escape(out, project_.overflow);
    out += ",\"result_order\":";
    pm_json_escape(out, project_.result_order);
    // Round-trip dispatch groups + default_group (F1 data-loss: omitting them
    // meant any instance CRUD silently wiped the user's group/priority/affinity
    // config). cpu_affinity is always written nested ([[...]]) — semantically
    // identical to the flat form the parser also accepts.
    if (!project_.groups.empty()) {
        out += ",\"groups\":[";
        for (size_t gi = 0; gi < project_.groups.size(); ++gi) {
            const auto& g = project_.groups[gi];
            if (gi) out += ",";
            out += "{\"name\":"; pm_json_escape(out, g.name);
            out += ",\"max_parallel\":" + std::to_string(g.max_parallel);
            out += ",\"thread_priority\":"; pm_json_escape(out, g.thread_priority);
            out += ",\"queue_depth\":" + std::to_string(g.queue_depth);
            out += ",\"overflow\":"; pm_json_escape(out, g.overflow);
            out += ",\"result_order\":"; pm_json_escape(out, g.result_order);
            out += ",\"min_interval_ms\":" + std::to_string(g.min_interval_ms);
            if (!g.cpu_affinity.empty()) {
                out += ",\"cpu_affinity\":[";
                for (size_t ai = 0; ai < g.cpu_affinity.size(); ++ai) {
                    if (ai) out += ",";
                    out += "[";
                    for (size_t ci = 0; ci < g.cpu_affinity[ai].size(); ++ci) {
                        if (ci) out += ",";
                        out += std::to_string(g.cpu_affinity[ai][ci]);
                    }
                    out += "]";
                }
                out += "]";
            }
            out += "}";
        }
        out += "]";
    }
    if (!project_.default_group.empty()) {
        out += ",\"default_group\":"; pm_json_escape(out, project_.default_group);
    }
    out += "},\n";
    // runtime block (process_priority + timer_fps) — also round-tripped (F1).
    if (!project_.runtime_priority.empty() || project_.runtime_timer_fps >= 0) {
        out += "  \"runtime\": {";
        bool rfirst = true;
        if (!project_.runtime_priority.empty()) {
            out += "\"process_priority\":"; pm_json_escape(out, project_.runtime_priority);
            rfirst = false;
        }
        if (project_.runtime_timer_fps >= 0) {
            if (!rfirst) out += ",";
            out += "\"timer_fps\":" + std::to_string(project_.runtime_timer_fps);
        }
        out += "},\n";
    }
    out += "  \"instances\": [";
    int i = 0;
    for (auto& [k, v] : project_.instances) {
        if (i++) out += ",";
        out += "\n    {\"name\": "; pm_json_escape(out, v.name);
        out += ", \"plugin\": ";   pm_json_escape(out, v.plugin_name);
        out += "}";
    }
    out += "\n  ]";
    // Round-trip the declarative plugin model (a save must not drop it).
    if (!project_.plugin_dirs.empty()) {
        out += ",\n  \"plugin_dirs\": [";
        for (size_t j = 0; j < project_.plugin_dirs.size(); ++j) {
            if (j) out += ", ";
            pm_json_escape(out, project_.plugin_dirs[j]);
        }
        out += "]";
    }
    if (!project_.plugins.empty()) {
        out += ",\n  \"plugins\": {";
        int j = 0;
        for (auto& p : project_.plugins) {
            if (j++) out += ",";
            out += "\n    "; pm_json_escape(out, p.label);
            out += ": {\"path\": "; pm_json_escape(out, p.path);
            out += ", \"compile\": " + std::string(p.compile ? "true" : "false") + "}";
        }
        out += "\n  }";
    }
    out += "\n}\n";
    // Preserve top-level keys owned by another writer (params / auto_respawn /
    // watchdog_ms / …) instead of clobbering them on every CRUD.
    out = merge_unknown_top_keys_(out, pj);
    // D-P1-5: atomic_write may fail (disk full / read-only / etc.).
    // Bubble that up — silently losing project.json was the audit
    // finding. Caller can't really recover, but at least logs it.
    if (!xi::atomic_write(pj, out)) {
        std::fprintf(stderr,
            "[xinsp2] save_project_locked: atomic_write failed for %s "
            "(disk full / read-only?). Project state on disk may be stale.\n",
            pj.string().c_str());
        return false;
    }
    return true;
}


inline bool PluginManager::save_instance_json(const InstanceInfo& ii) {
    auto path = std::filesystem::path(ii.folder_path) / "instance.json";
    std::string out = "{\n";
    out += "  \"plugin\": "; pm_json_escape(out, ii.plugin_name); out += ",\n";
    // Preserve the per-instance concurrency cap across saves (else a UI save
    // would silently drop a hand-set max_concurrency).
    if (ii.max_concurrency > 0)
        out += "  \"max_concurrency\": " + std::to_string(ii.max_concurrency) + ",\n";
    // Round-trip the dispatch group (F3: a UI save / create / rename used to
    // drop it, so the source's triggers silently fell back to default_group).
    if (!ii.group.empty()) {
        out += "  \"group\": "; pm_json_escape(out, ii.group); out += ",\n";
    }
    // item 14: round-trip a per-instance on_fault OVERRIDE — only when it differs
    // from the plugin default (so a save doesn't drop a user's override, nor bake
    // the plugin default into every instance.json).
    {
        OnFault plugin_dflt = OnFault::Reuse;
        if (auto pit = plugins_.find(ii.plugin_name); pit != plugins_.end())
            plugin_dflt = pit->second.default_on_fault;
        if (ii.on_fault != plugin_dflt) {
            out += "  \"on_fault\": \"";
            out += on_fault_name(ii.on_fault);
            out += "\",\n";
        }
    }
    out += "  \"config\": ";
    out += ii.instance ? ii.instance->get_def() : "{}";
    out += "\n}\n";
    // D-P1-5: surface atomic_write failures so the user knows their
    // edit didn't reach disk.
    if (!xi::atomic_write(path, out)) {
        std::fprintf(stderr,
            "[xinsp2] save_instance_json: atomic_write failed for %s "
            "(disk full / read-only?). Instance config on disk may be stale.\n",
            path.string().c_str());
        return false;
    }
    return true;
}

} // namespace xi
