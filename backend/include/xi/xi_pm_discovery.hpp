#pragma once
//
// xi_pm_discovery.hpp — PluginManager plugin discovery / certification /
// registration / external-plugin resolution.
//
// Out-of-class inline definitions for the "find & register plugins" concern:
//   scan_plugins / set_certify_exe / certify_warnings / unquarantine_plugin
//   register_plugin_folder_locked_ / expand_plugin_root_
//   resolve_external_project_plugins_locked_ / compile_project_plugins
//   certify_folder_locked_
//
// Mechanically extracted from xi_plugin_manager.hpp — bodies are byte-identical
// to the former in-class definitions.
//
#include "xi_pm_manager_core.hpp"

#include <algorithm>          // std::remove_if (unquarantine_plugin)
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace xi {

// Scan a directory for plugin folders. Each subfolder with a plugin.json
// is registered. An already-loaded plugin (handle != nullptr) keeps its
// handle and resolved factory — we refresh only manifest metadata so
// rescan_plugins doesn't leak the prior HMODULE.
inline int PluginManager::scan_plugins(const std::string& plugins_dir) {
    if (!std::filesystem::exists(plugins_dir)) return 0;

    // J6: the certify step per changed-hash plugin spawns a throwaway child that
    // blocks up to 30s (WaitForSingleObject). Running it under mu_ stalls the bus
    // sink's instance_group() (same mu_) on the source emit hot path for up to
    // 30s/plugin during a live rescan. So do the subprocess work in an UNLOCKED
    // pre-pass that only WARMS the on-disk verdict cache; the locked scan below is
    // unchanged and its certify_folder_locked_ then hits the fresh cache without
    // spawning. certify_exe_ is set once at startup (set_certify_exe, mu_-guarded)
    // and never mutated during a scan — snapshot it under a brief lock so the
    // off-lock precertify_folder_ never touches the member.
    std::string certify_exe;
    { std::lock_guard<std::mutex> lk(mu_); certify_exe = certify_exe_; }
    for (auto& entry : std::filesystem::directory_iterator(plugins_dir)) {
        if (!entry.is_directory()) continue;
        const std::string folder = entry.path().string();
        auto manifest = std::filesystem::path(folder) / "plugin.json";
        if (!std::filesystem::exists(manifest)) continue;
        auto info = parse_manifest(manifest.string(), folder);
        if (info.name.empty()) continue;
        precertify_folder_(folder, info, certify_exe);   // up-to-30s subprocess, OFF mu_
    }

    std::lock_guard<std::mutex> lk(mu_);
    int count = 0;
    for (auto& entry : std::filesystem::directory_iterator(plugins_dir)) {
        if (!entry.is_directory()) continue;
        const std::string folder = entry.path().string();
        // Part III G1.3 — gate discovery on the cached certify verdict. A
        // plugin whose last certification CRASHED a throwaway child process
        // (its DllMain/factory faulted) is SKIPPED + surfaced, so scanning
        // itself can never re-arm a known-bad DLL inside the backend. Only
        // the `crashed` verdict gates — abi_mismatch/unknown still register
        // (the real load path refuses the former with a reason; the latter
        // means "not yet certifiable", e.g. a source-only plugin not built).
        auto manifest = std::filesystem::path(folder) / "plugin.json";
        if (std::filesystem::exists(manifest)) {
            auto info = parse_manifest(manifest.string(), folder);
            if (!info.name.empty()) {
                // G1.3 `crashed` (certify child faulted at discovery) AND G2.2
                // `quarantined` (the FE attributed N runtime crashes to this
                // plugin) BOTH gate discovery through this one cache+gate — a
                // known-bad DLL is never armed inside the backend. The two
                // differ only in the operator-facing reason. Rebuilding the DLL
                // (new content hash) clears either (G1.2 / G2.3).
                auto verdict = certify_folder_locked_(folder, info);
                if (verdict == certify::Verdict::crashed) {
                    std::string reason =
                        "plugin '" + info.name + "' SKIPPED at discovery: certification "
                        "crashed a throwaway child process (malformed DLL — DllMain or "
                        "factory faults). Fix + rebuild the DLL to re-certify.";
                    std::fprintf(stderr, "[xinsp2] %s\n", reason.c_str());
                    last_certify_warnings_.push_back({info.name, info.name, reason});
                    gated_folders_[info.name] = folder;   // G2.3 un-quarantine lookup
                    continue;   // do NOT register/arm a known-bad DLL
                }
                if (verdict == certify::Verdict::quarantined) {
                    std::string reason =
                        "plugin '" + info.name + "' QUARANTINED + disabled: it was "
                        "attributed repeated backend crashes at runtime. The line stays "
                        "up with this plugin off. Rebuild the DLL (or un-quarantine via "
                        "cmd:unquarantine_plugin) once it is fixed.";
                    std::fprintf(stderr, "[xinsp2] %s\n", reason.c_str());
                    last_certify_warnings_.push_back({info.name, info.name, reason});
                    gated_folders_[info.name] = folder;   // G2.3 un-quarantine lookup
                    continue;   // do NOT register/arm a quarantined DLL
                }
            }
        }
        if (register_plugin_folder_locked_(folder)) count++;
    }
    return count;
}

// The exe used to certify a plugin in a throwaway child (any binary that
// handles `--certify-plugin <dir>`: the backend, the runner, or a test
// driver). Empty (the default) DISABLES running new certifications; scan
// then still honours any verdict already cached on disk (so a known-bad DLL
// stays gated) but never spawns a child. Set it at startup before scanning.
inline void PluginManager::set_certify_exe(const std::string& exe) {
    std::lock_guard<std::mutex> lk(mu_);
    certify_exe_ = exe;
}

// Certify warnings from the most recent scan_plugins (plugins skipped because
// their cached verdict was `crashed`). Separate from open_warnings() — those
// are per-open_project and get cleared each open; these survive discovery.
inline std::vector<OpenWarning> PluginManager::certify_warnings() {
    std::lock_guard<std::mutex> lk(mu_);
    return last_certify_warnings_;
}

// Part III G2.3 — operator un-quarantine. Clears the certify/quarantine verdict
// for a gated plugin (by name, resolved to its folder via the last scan, or by
// an explicit folder) by REMOVING G1's .xi_certify.json — the next scan then
// re-certifies from scratch (a still-bad DLL re-gates itself; a genuinely-fixed
// one arms). This is the manual override; the automatic path is a DLL rebuild,
// whose new content hash already invalidates the cached verdict (G1.2). Returns
// true if a cache file was found + removed. Reuses the ONE quarantine mechanism
// (Invariant §20.3) — no separate enable flag.
inline bool PluginManager::unquarantine_plugin(const std::string& name_or_folder) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string folder;
    if (auto it = gated_folders_.find(name_or_folder); it != gated_folders_.end())
        folder = it->second;
    else if (std::filesystem::exists(std::filesystem::path(name_or_folder) / "plugin.json"))
        folder = name_or_folder;   // caller passed the folder directly
    else if (auto* pi = (plugins_.count(name_or_folder) ? &plugins_[name_or_folder] : nullptr))
        folder = pi->folder_path;  // a currently-registered plugin
    if (folder.empty()) return false;
    std::error_code ec;
    bool removed = std::filesystem::remove(certify::cache_path(folder), ec);
    gated_folders_.erase(name_or_folder);
    // Drop the stale gate warning(s) for this plugin so a follow-up scan's
    // surface is clean.
    last_certify_warnings_.erase(
        std::remove_if(last_certify_warnings_.begin(), last_certify_warnings_.end(),
                       [&](const OpenWarning& w) { return w.plugin == name_or_folder; }),
        last_certify_warnings_.end());
    return removed || true;   // folder resolved = success even if no cache existed
}

// Register a single plugin folder (one that contains plugin.json) into the
// registry. An already-loaded plugin of the same name keeps its live handle +
// factories — we refresh only the metadata that can change between scans, so
// re-registering doesn't leak the prior HMODULE. mu_ MUST be held.
//
// track_as_project: this folder is a compile:false EXTERNAL resolved for the
// OPEN PROJECT (vs. a global plugins-dir scan). #9: such externals must be
// recorded in project_loaded_plugins_ so close/open frees their HMODULE — the
// teardown used to only free origin (compiled) plugins, so a compile:false
// external survived into the next project and ran its OLD DLL's code.
inline bool PluginManager::register_plugin_folder_locked_(const std::string& folder,
                                                          bool track_as_project) {
    auto manifest = std::filesystem::path(folder) / "plugin.json";
    if (!std::filesystem::exists(manifest)) return false;
    auto info = parse_manifest(manifest.string(), folder);
    if (info.name.empty()) return false;
    std::string key = info.name;   // capture before any move
    auto existing = plugins_.find(info.name);
    if (existing != plugins_.end() && existing->second.handle) {
        // #9: if the resolved folder/dll MOVED, the loaded handle points at a
        // DIFFERENT plugin than the one now being registered — keeping it would
        // run the old DLL's code. Drop + FreeLibrary and fall through to a fresh
        // registration so the next load_plugin() maps the new DLL.
        bool moved = (existing->second.folder_path != info.folder_path) ||
                     (existing->second.dll_name     != info.dll_name);
        if (moved) {
            // SAFETY GUARD (UAF): live CAbiInstanceAdapters (project instances
            // and machine-autoload providers) cache raw GetProcAddress pointers
            // into this HMODULE. FreeLibrary-ing it while any such consumer is
            // alive unmaps the module under them — the next dispatch or adapter
            // dtor then calls into unmapped memory (SEGV). This branch is only
            // reachable via a deliberate on-disk move/rename of an in-use
            // plugin (e.g. an in-place prebuilt upgrade that edits "dll" in
            // plugin.json), so REFUSE the unload+swap here instead of paying
            // for a full detach→FreeLibrary→reattach rebuild: keep the old,
            // still-mapped handle and registration, and tell the operator how
            // to adopt the new binary. Same in-use predicates the autoload /
            // reload paths use (see xi_pm_load.hpp).
            bool in_use = project_provides_plugin_locked_(info.name) ||
                          machine_instances_.count(info.name) > 0;
            if (in_use) {
                std::fprintf(stderr,
                    "[xinsp2] plugin '%s' moved on disk (%s/%s -> %s/%s) but is IN USE "
                    "by a live project instance or machine provider — REFUSING the "
                    "in-place unload+swap (unloading now would leave live adapters "
                    "calling into an unmapped DLL). The previously loaded binary stays "
                    "active at its old location. Close the project / evict the machine "
                    "provider, or recompile via the reload path, then rescan to adopt "
                    "the new binary.\n",
                    info.name.c_str(),
                    existing->second.folder_path.c_str(), existing->second.dll_name.c_str(),
                    info.folder_path.c_str(), info.dll_name.c_str());
                // Do NOT touch the entry (its folder/dll must keep describing the
                // module actually mapped) and do NOT tag it project-tracked here —
                // retagging could let close_project FreeLibrary a handle a machine
                // provider still holds. The plugin stays registered and valid.
                return true;
            }
            FreeLibrary(existing->second.handle);   // TODO(linux): dlclose
            existing->second.handle    = nullptr;
            existing->second.c_factory = nullptr;
            plugins_[info.name] = std::move(info);
        } else {
            existing->second.description   = info.description;
            existing->second.has_ui        = info.has_ui;
            existing->second.reentrant     = info.reentrant;
            existing->second.is_sink       = info.is_sink;
            existing->second.default_on_fault = info.default_on_fault;
            existing->second.json_fallback = info.json_fallback;
            existing->second.prebuilt      = info.prebuilt;
            existing->second.ui_path       = info.ui_path;
            existing->second.folder_path   = info.folder_path;
            existing->second.manifest_json = info.manifest_json;
        }
    } else {
        plugins_[info.name] = std::move(info);
    }
    if (track_as_project) project_loaded_plugins_.insert(key);
    return true;
}

// Expand a project.json plugin_dir entry to an absolute search root. Portable
// forms only: `${ENV}` substitution, leading `~`, and relative paths (resolved
// against the project folder). Absolute paths pass through. This is what keeps
// the committed project.json machine-independent — absolute machine roots live
// in env vars, not in the file.
inline std::string PluginManager::expand_plugin_root_(std::string r, const std::string& project_folder) {
    for (size_t p; (p = r.find("${")) != std::string::npos; ) {
        auto e = r.find('}', p);
        if (e == std::string::npos) break;
        std::string var = r.substr(p + 2, e - p - 2);
        const char* val = std::getenv(var.c_str());
        r.replace(p, e - p + 1, val ? val : "");
    }
    if (!r.empty() && r[0] == '~') {
        const char* home = std::getenv("USERPROFILE");      // TODO(linux): $HOME
        if (!home) home = std::getenv("HOME");
        if (home) r = std::string(home) + r.substr(1);
    }
    std::filesystem::path p(r);
    if (p.is_relative()) p = std::filesystem::path(project_folder) / p;
    return p.lexically_normal().string();
}

// Resolve project.json's external plugin coordinates: for each `plugins`
// entry's `path`, search each (expanded) `plugin_dirs` root in order and take
// the first `<root>/<path>/plugin.json` found (makefile-/$PATH-style
// first-match-wins). Per ref, `compile` decides the treatment:
//   false = only REGISTER (must be prebuilt or build:cmake);
//   true  = treat the folder exactly like a `<project>/plugins/` one —
//           cl.exe-compile source / load cmake-prebuilt, TRUSTED (no cert),
//           recompile/rebuild-able. The "plugin toolbox" dev case.
// Unresolved refs become open warnings listing the roots searched. Runs AFTER
// project-local plugins are built so a same-named project plugin wins. Relative
// roots resolve against the project folder (so a local `plugins` root needs no
// `./`). mu_ MUST be held.
inline void PluginManager::resolve_external_project_plugins_locked_(
        const std::string& project_folder,
        const std::vector<std::string>& dirs_raw,
        const std::vector<ProjectInfo::PluginRef>& refs) {
    if (refs.empty()) return;
    std::vector<std::string> roots;
    roots.reserve(dirs_raw.size());
    for (auto& d : dirs_raw) roots.push_back(expand_plugin_root_(d, project_folder));
    std::vector<std::filesystem::directory_entry> to_compile;
    for (auto& ref : refs) {
        // SECURITY (P1): ref.path is VERBATIM from project.json and is joined
        // onto each search root below — an absolute value DISCARDS the root
        // (operator/ semantics) and a "../" chain climbs out of it, so a
        // semi-trusted project folder could LoadLibrary a pre-planted DLL
        // from an arbitrary machine path (only plugin_abi_compatible gates
        // it). Require a contained relative path; the check is lexical, so a
        // passing ref.path stays inside WHICHEVER root resolves it. The
        // plugin_dirs roots themselves are NOT constrained: an absolute root
        // (machine-wide toolbox via ${ENV}/~, see expand_plugin_root_) is
        // host-side configuration, not project-embedded data.
        if (!path_is_contained(std::filesystem::path(project_folder), ref.path)) {
            std::string msg =
                "plugin path '" + ref.path + "' is absolute or escapes the "
                "project tree ('..') — SKIPPED by the path-containment guard "
                "(project.json plugins[].path must be relative and resolve "
                "inside a declared plugin_dir; see path_is_contained)";
            last_open_warnings_.push_back({ref.label, "", msg});
            std::fprintf(stderr, "[xinsp2] plugin '%s': %s\n",
                         ref.label.c_str(), msg.c_str());
            continue;
        }
        std::filesystem::path found;
        for (auto& root : roots) {
            auto cand = std::filesystem::path(root) / ref.path;
            if (std::filesystem::exists(cand / "plugin.json")) { found = cand; break; }
        }
        if (found.empty()) {
            std::string searched;
            for (auto& root : roots)
                searched += "\n  - " + (std::filesystem::path(root) / ref.path).string();
            last_open_warnings_.push_back({ref.label, "",
                "plugin '" + ref.path + "' not found in any plugin_dir; searched:" + searched +
                (roots.empty() ? "\n  (no plugin_dirs declared)" : "")});
            std::fprintf(stderr, "[xinsp2] plugin '%s' (%s) not resolved against %zu plugin_dir(s)\n",
                         ref.label.c_str(), ref.path.c_str(), roots.size());
            continue;
        }
        std::fprintf(stderr, "[xinsp2] resolved plugin '%s' -> %s%s\n",
                     ref.label.c_str(), found.string().c_str(), ref.compile ? " (compile)" : "");
        if (ref.compile) {
            to_compile.emplace_back(found);   // build + register as a trusted project plugin
        } else if (!register_plugin_folder_locked_(found.string(), /*track_as_project=*/true)) {
            last_open_warnings_.push_back({ref.label, "",
                "resolved folder has no valid plugin.json: " + found.string()});
        }
    }
    if (!to_compile.empty()) compile_plugin_folders_locked_(to_compile);
}

inline int PluginManager::compile_project_plugins(const std::string& project_folder) {
    std::lock_guard<std::mutex> lk(mu_);
    return compile_project_plugins_locked(project_folder);
}

// Part III G1.1/G1.2 — certify a plugin folder, cached by DLL content hash.
// Re-certifies (spawns a throwaway child via certify_exe_) ONLY when the DLL
// hash is missing/changed; otherwise reuses the verdict cached next to the
// manifest, so a normal startup isn't slowed. Returns `unknown` (never
// gating) when there is no built DLL yet, or when no certify_exe_ is wired
// and no cache exists. mu_ MUST be held.
inline certify::Verdict PluginManager::certify_folder_locked_(const std::string& folder,
                                                              const PluginInfo& info) {
    auto dll_path = (std::filesystem::path(folder) / info.dll_name).string();
    std::string hash = xi::sha256::sha256_file(dll_path);
    if (hash.empty()) return certify::Verdict::unknown;   // source-only / unbuilt — nothing to certify

    certify::CacheEntry cached;
    if (certify::read_cache(folder, cached) &&
        cached.dll == info.dll_name && cached.sha256 == hash) {
        return certify::verdict_from_str(cached.verdict);  // G1.2 — hash unchanged, reuse
    }

    if (certify_exe_.empty()) return certify::Verdict::unknown;   // certification disabled

    // certify::run_certify_subprocess is cross-platform (CreateProcess on Windows,
    // fork+execl on POSIX — see xi_certify.hpp), so the scan-time gate runs on both.
    auto verdict = certify::run_certify_subprocess(certify_exe_, folder);
    if (verdict != certify::Verdict::unknown)   // don't cache a spawn failure
        certify::write_cache(folder, { info.dll_name, hash, certify::verdict_str(verdict) });
    return verdict;
}

// J6 off-lock cache-warm — mirrors certify_folder_locked_'s decision, but spawns
// the up-to-30s certify child WITHOUT mu_ (certify_exe passed as a snapshot). It
// only reads/writes the on-disk verdict cache; scan_plugins runs it in an unlocked
// pre-pass so the locked certify_folder_locked_ then finds the cache fresh (hash
// unchanged, G1.2) and returns without spawning under the lock. No-op when the DLL
// hash is unresolvable, the cache is already fresh, or no certify exe is wired.
inline void PluginManager::precertify_folder_(const std::string& folder,
                                              const PluginInfo& info,
                                              const std::string& certify_exe) {
    auto dll_path = (std::filesystem::path(folder) / info.dll_name).string();
    std::string hash = xi::sha256::sha256_file(dll_path);
    if (hash.empty()) return;                       // source-only / unbuilt — nothing to certify

    certify::CacheEntry cached;
    if (certify::read_cache(folder, cached) &&
        cached.dll == info.dll_name && cached.sha256 == hash)
        return;                                     // G1.2 — hash unchanged, cache already fresh

    if (certify_exe.empty()) return;                // certification disabled

    // Cross-platform certify child (see certify_folder_locked_ above).
    auto verdict = certify::run_certify_subprocess(certify_exe, folder);
    if (verdict != certify::Verdict::unknown)       // don't cache a spawn failure
        certify::write_cache(folder, { info.dll_name, hash, certify::verdict_str(verdict) });
}

} // namespace xi
