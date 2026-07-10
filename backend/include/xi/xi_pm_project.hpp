#pragma once
//
// xi_pm_project.hpp — PluginManager project lifecycle + working-copy concern.
//
// Out-of-class inline definitions for:
//   create_project / close_project / commit_working_copy /
//   reopen_fresh_working_copy / open_project
//
// open_project also parses project.json (parallelism / runtime / plugin_dirs /
// plugins / instances) and drives project-plugin compile + external resolution.
//
// Mechanically extracted from xi_plugin_manager.hpp — bodies are byte-identical
// to the former in-class definitions.
//
#include "xi_pm_manager_core.hpp"
#include "xi_project.hpp"     // kProjectSchema* + parse_project_schema (loader gate)

#include <algorithm>          // std::min/std::max (group clamps)
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace xi {

inline bool PluginManager::create_project(const std::string& folder, const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    std::filesystem::create_directories(folder);
    std::filesystem::create_directories(std::filesystem::path(folder) / "instances");

    // Drop any stale folder/registry entries from a previous project
    for (auto& [k, v] : project_.instances) {
        InstanceRegistry::instance().remove(k);
        InstanceFolderRegistry::instance().clear(k);
    }
    project_.name = name;
    project_.folder_path = folder;
    project_.script_path = (std::filesystem::path(folder) / "inspect.cpp").string();
    project_.instances.clear();
    // New projects start with the local ./plugins root visible + an empty
    // plugins map; declare each plugin (or add it from the Plugin Browser).
    project_.plugin_dirs = {"./plugins"};
    project_.plugins.clear();

    // Write initial project.json
    save_project_locked();

    // Create a starter inspect.cpp if it doesn't exist
    if (!std::filesystem::exists(project_.script_path)) {
        std::string body =
            "// " + name + " — inspection script\n"
            "#include <xi/xi.hpp>\n"
            "// xi.hpp is OpenCV-free. For image ops add #include <xi/xi_cv.hpp>\n"
            "// and call cv:: on xi::as_cv_mat(img) / Image::create_in_pool().\n\n"
            "XI_SCRIPT_EXPORT\n"
            "void xi_inspect_entry(int frame) {\n"
            "    // TODO: add inspection logic\n"
            "}\n";
        xi::atomic_write(project_.script_path, body);
    }
    return true;
}

inline void PluginManager::close_project() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [k, v] : project_.instances) {
        InstanceRegistry::instance().remove(k);
        InstanceFolderRegistry::instance().clear(k);
    }
    // Destroy instances FIRST — same constraint as open_project (line
    // ~995): CAbiInstanceAdapter's destructor calls the plugin's
    // destroy_fn, which lives in the project plugin's DLL. FreeLibrary
    // before the adapter dies leaves the destructor calling a dangling
    // function pointer and the backend SEGVs. Clearing project_
    // instances explicitly here lets us choose the order; the
    // subsequent project_ = ProjectInfo{} would only destroy them
    // after the FreeLibrary calls below if we relied on aggregate
    // assignment.
    // FL r8 P1 reproducer: harness_open_close_cycle.py iter 0.
    project_.instances.clear();
    inst_state_.clear();   // host-tracked lifecycle state dies with the instances
    // Now safe to drop the previous project's plugins — adapters are
    // gone, no live destroy_fn callers remain.
    // #9/#13: free EVERY plugin this project loaded — compiled project plugins
    // AND compile:false externals AND manifest-renamed ones — by the SAME key
    // that holds the HMODULE in plugins_ (the manifest name). The prior loop
    // iterated project_plugin_origin_ (folder-keyed, externals absent) and so
    // missed renamed handles + every external, leaking the HMODULE and leaving a
    // stale entry that shadowed the next project's same-named plugin.
    for (auto& key : project_loaded_plugins_) {
        auto it = plugins_.find(key);
        if (it != plugins_.end()) {
            if (it->second.handle) FreeLibrary(it->second.handle);   // TODO(linux): dlclose
            plugins_.erase(it);
        }
    }
    project_loaded_plugins_.clear();
    project_plugin_origin_.clear();
    project_ = ProjectInfo{};
    // V3: machine-autoloaded lib providers are machine-scoped — they survive the
    // project teardown above (never in project_.instances / project_loaded_
    // plugins_). Reinstate any that THIS project had displaced with its own
    // instance (their global DLL is still mapped in plugins_), so a capability
    // that was project-provided stays available after close. No-op at boot / when
    // nothing was displaced.
    autoload_machine_providers_locked_();
}

// ---- working copy (transactional edits at <project>/.xinsp_work) --------
// Constants + the filesystem mechanics (seed/mirror/exclude/gitignore) live
// in xi_working_copy.hpp; the aliases in the core header keep the references
// terse. The stateful transactional methods (open/commit/discard) stay here.

// Commit: mirror the working copy back onto the canonical project (adds +
// overwrites + deletes removed files), so the on-disk project reflects every
// edit made this session. No-op error if no working copy is active.
inline bool PluginManager::commit_working_copy() {
    std::lock_guard<std::mutex> lk(mu_);
    if (canonical_path_.empty()) return false;
    // Journal the commit so an interruption (crash/power loss mid-mirror) is
    // detectable + recoverable. The scratch is never modified by the mirror,
    // so it stays a complete snapshot; mirror_tree_ is idempotent, so the
    // next open_project can roll a partial commit forward from it.
    std::error_code ec;
    std::filesystem::path marker = std::filesystem::path(canonical_path_) / kCommitMarker;
    // F4: the marker MUST be durable on disk before the mirror starts — a bare
    // ofstream leaves it in the OS cache, so a power loss mid-mirror could tear
    // the canonical with no marker to drive roll-forward. atomic_write flushes
    // (FlushFileBuffers + atomic rename). If even the marker can't be written,
    // abort rather than mirror without a journal.
    if (!xi::atomic_write(marker, std::string("commit in progress\n"))) {
        std::fprintf(stderr, "[xinsp2] working copy: commit-marker write failed — aborting commit\n");
        return false;
    }
    // If the mirror hit a disk error (full disk, read-only canonical, a locked
    // destination file), the canonical tree is torn. Do NOT remove the marker:
    // leaving it makes the next open_project roll the (intact) scratch forward
    // and heal the canonical. Report failure rather than a false "committed".
    if (!xi::wc::mirror_tree(project_.folder_path, canonical_path_)) {
        std::fprintf(stderr, "[xinsp2] working copy: commit mirror FAILED (disk error?) "
                     "— marker kept for roll-forward on next open\n");
        return false;
    }
    std::filesystem::remove(marker, ec);   // commit complete -> clear journal
    std::fprintf(stderr, "[xinsp2] working copy: committed to %s\n",
                 canonical_path_.c_str());
    return true;
}

// Discard: blow away the working copy and re-seed it from the canonical
// project, then reopen. Returns false if no working copy is active.
inline bool PluginManager::reopen_fresh_working_copy() {
    std::string canon;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (canonical_path_.empty()) return false;
        canon = canonical_path_;
    }
    // close_project()/open_project() each take mu_ — don't hold it here.
    close_project();
    std::error_code ec;
    // CRASH-RECOVERY GUARD (bug #14). Discard's contract is "throw away my
    // UNCOMMITTED working-copy edits". A *pending commit* (kCommitMarker
    // present) is NOT uncommitted edits — it is a half-applied commit the user
    // already asked for, with the canonical possibly torn and the intact scratch
    // its ONLY heal source. Blindly remove_all(scratch) here (the old behaviour)
    // destroys that snapshot before open_project's roll-forward can heal the
    // canonical → the torn canonical becomes permanently unrecoverable.
    //
    // So COMPLETE the interrupted commit from the scratch FIRST, via the same
    // heal used on open. (Rolling the commit BACK isn't possible — the
    // pre-commit canonical bytes are already partially overwritten — so
    // completing the commit the user requested is the correct, least-surprising
    // behaviour.) Only once the canonical is healed (or there was no pending
    // commit) is it safe to drop the scratch and re-seed a fresh working copy.
    // If the heal mirror itself fails (persistent disk error), KEEP the scratch
    // + marker so the reopen below (and future opens) can retry the
    // roll-forward — never discard the only recovery source.
    if (xi::wc::roll_forward_pending_commit(canon)) {
        std::filesystem::remove_all(std::filesystem::path(canon) / kWorkingCopyDir, ec);
    } else {
        std::fprintf(stderr, "[xinsp2] working copy: discard KEPT scratch — pending "
                     "commit heal failed; reopen will retry roll-forward\n");
    }
    return open_project(canon, /*working_copy=*/true);   // re-seeds from canonical
}

inline bool PluginManager::open_project(const std::string& folder_arg, bool working_copy) {
    std::lock_guard<std::mutex> lk(mu_);

    // Roll forward an interrupted working-copy commit before touching
    // anything: if the canonical carries the commit-pending marker, a prior
    // commit was cut short (crash/power loss) and the canonical tree may be
    // torn. The scratch is a complete, untouched snapshot, so re-running the
    // (idempotent) mirror finishes the commit and heals the canonical. The
    // heal step is factored into xi::wc::roll_forward_pending_commit so the
    // Discard path (reopen_fresh_working_copy) heals via the SAME logic
    // before it drops the scratch.
    xi::wc::roll_forward_pending_commit(folder_arg);

    // Working-copy mode: operate on a scratch copy at <project>/.xinsp_work
    // so edits never touch the canonical project until an explicit commit
    // (and survive a backend crash — the scratch is on disk). Resume an
    // existing scratch (crash recovery / unsaved session); otherwise seed it
    // from the canonical project. `folder` is then rebased to the scratch so
    // ALL downstream logic (compile, instances, saves) uses the working copy.
    std::string folder = folder_arg;
    canonical_path_.clear();
    if (working_copy) {
        std::filesystem::path canon = folder_arg;
        if (!std::filesystem::exists(canon / "project.json")) return false;
        std::filesystem::path scratch = canon / kWorkingCopyDir;
        if (!std::filesystem::exists(scratch / "project.json")) {
            // Honour the seed result the SAME way commit_working_copy honours
            // mirror_tree's: a swallowed copy failure (disk full, locked/denied
            // file) would leave a TORN scratch that still happens to carry
            // project.json — the exists() guard below would then accept it as
            // authoritative, the user edits it, and the eventual commit's
            // mirror_tree PRUNES the canonical files that merely failed to copy
            // in. That's silent data loss. seed_working_copy returns false on a
            // failed copy AND has already removed the partial scratch (so no
            // later crash-resume / resume-existing path can adopt it) — we then
            // abort the working-copy open rather than present a torn scratch.
            if (!xi::wc::seed_working_copy(canon, scratch)) {
                std::fprintf(stderr, "[xinsp2] working copy: seed of %s FAILED "
                             "(disk full / locked file?) — removed partial scratch, "
                             "aborting working-copy open\n", scratch.string().c_str());
                return false;
            }
            std::fprintf(stderr, "[xinsp2] working copy: seeded %s from project\n",
                         scratch.string().c_str());
        } else {
            std::fprintf(stderr, "[xinsp2] working copy: resuming existing %s\n",
                         scratch.string().c_str());
        }
        xi::wc::ensure_gitignore(canon, std::string(kWorkingCopyDir) + "/");
        canonical_path_ = canon.string();
        folder = scratch.string();
    }

    auto pj = std::filesystem::path(folder) / "project.json";
    if (!std::filesystem::exists(pj)) return false;

    // Unregister old instances from the global registries.
    for (auto& [k, v] : project_.instances) {
        InstanceRegistry::instance().remove(k);
        InstanceFolderRegistry::instance().clear(k);
    }
    // Destroy old instances FIRST — CAbiInstanceAdapter's destructor
    // calls its plugin's destroy_fn, which lives in the project
    // plugin's DLL. If we FreeLibrary the DLL before the adapter
    // dies (the prior order did), the destructor calls a dangling
    // function pointer and SEGVs the backend on a reopen.
    project_.instances.clear();

    // Now safe to drop the previous project's plugins — adapters
    // are gone, no live destroy_fn callers remain.
    // #9/#13: free EVERY plugin this project loaded by the manifest-name key
    // that holds the HMODULE (see close_project for the rationale) — externals
    // + renamed plugins included, so the prior project leaves no stale entry to
    // shadow this one's same-named plugin.
    for (auto& key : project_loaded_plugins_) {
        auto it = plugins_.find(key);
        if (it != plugins_.end()) {
            if (it->second.handle) FreeLibrary(it->second.handle);   // TODO(linux): dlclose
            plugins_.erase(it);
        }
    }
    project_loaded_plugins_.clear();
    project_plugin_origin_.clear();
    project_.folder_path = folder;

    // Parse project.json
    std::ifstream f(pj.string());
    std::stringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    // Minimal parsing — extract name, script, plugins, instances
    auto name_opt = extract_string(content, "name");
    if (name_opt) project_.name = *name_opt;
    auto script_opt = extract_string(content, "script");
    // SECURITY (P1): `script` is VERBATIM from project.json and becomes the
    // cl.exe SOURCE for the project compile — joined with operator/, an
    // absolute value discards the project folder and a "../" chain climbs out
    // of it, so a semi-trusted project could compile+run an out-of-tree,
    // pre-planted source file. Refuse the open loudly (same fail-loud shape
    // as the schema gate below: last_open_error_ + stderr + return false)
    // rather than degrade — a project whose script points outside its own
    // tree is hostile or broken either way. See path_is_contained
    // (xi_pm_parse.hpp) for the guard + threat model.
    if (script_opt &&
        !path_is_contained(std::filesystem::path(folder), *script_opt)) {
        last_open_error_ =
            "project.json 'script' (\"" + *script_opt + "\") is absolute or "
            "escapes the project folder ('..') — refusing to open: the "
            "inspection script must live inside the project tree "
            "(path-containment guard; an out-of-tree script would be compiled "
            "and executed from an arbitrary machine path).";
        std::fprintf(stderr, "[xinsp2] %s\n", last_open_error_.c_str());
        return false;
    }
    if (script_opt) project_.script_path = (std::filesystem::path(folder) / *script_opt).string();
    else            project_.script_path = (std::filesystem::path(folder) / "inspect.cpp").string();

    // Parse trigger_policy block (optional; older project.json files
    // have none and we default to Any). Use a JSON parser instead of
    // substring search — the previous string-scan version assumed
    // `"required":[` with no whitespace and silently dropped the
    // list when a tool / human formatted the JSON with a space
    // after the colon (Python's json.dumps default).
    project_.dispatch_threads  = 1;
    project_.queue_depth       = 100;
    project_.max_inflight      = 64;
    project_.overflow          = "drop_oldest";
    project_.result_order      = "completion";
    project_.groups.clear();
    project_.default_group.clear();
    project_.runtime_priority.clear();
    project_.runtime_timer_fps = -1;
    // Reset surfaced warnings here (before the project.json parse) so group
    // parse warnings, compile failures, and bad instances all accumulate.
    last_open_warnings_.clear();
    // Reset any prior hard-refusal reason (unrecognized schema) so it never
    // leaks into this open's result.
    last_open_error_.clear();
    // Was the top-level project.json itself well-formed? A malformed file
    // (truncated / garbage) used to load "successfully" with all defaults and
    // no signal to the user — surface it as an open warning below.
    bool project_json_malformed = false;
    // External plugin coordinates (resolved after project-local plugins build):
    // plugin_dirs = ordered search roots; plugins = { label: { path } } refs.
    std::vector<std::string> proj_plugin_dirs;
    std::vector<ProjectInfo::PluginRef> proj_plugin_refs;
    // H2: {name, plugin} entries declared in project.json's top-level `instances`
    // array. Instances only materialize from instances/<name>/instance.json
    // (scanned below); a project.json-only entry is inert. Collected here so the
    // post-scan cross-check can warn loudly about any phantom (no backing dir).
    std::vector<std::pair<std::string, std::string>> proj_declared_instances;
    // Project-level DEFAULT for a `plugins` entry that omits its own `compile`
    // flag; per-entry `compile` (parsed below) overrides it. Default off.
    bool proj_plugin_dirs_compile = json_flag_true(content, "plugin_dirs_compile");
    yyjson_doc* doc = yyjson_read(content.c_str(), content.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (root) {
        // Schema-identity gate (Finding 2 / adoption item 12) — same discipline
        // as the plugin ABI load gate: a MISSING schema is accepted as a legacy
        // (pre-schema) file and logged once; a RECOGNIZED family whose major is
        // this backend's (or older) loads normally; an unrecognized family or a
        // FUTURE major is REFUSED with both versions named, rather than silently
        // mis-read. A save later stamps the current schema onto a legacy file.
        if (yyjson_val* sv = yyjson_obj_get(root, "schema");
            sv && yyjson_is_str(sv) && yyjson_get_str(sv)) {
            std::string sch = yyjson_get_str(sv);
            int major = 0;
            bool recognized = xi::project::parse_project_schema(sch, major);
            if (!recognized || major > xi::project::kProjectSchemaMajor) {
                last_open_error_ =
                    "project.json declares schema \"" + sch + "\" but this backend "
                    "understands \"" + std::string(xi::project::kProjectSchema) +
                    "\" (family " + std::string(xi::project::kProjectSchemaFamily) +
                    ", major " + std::to_string(xi::project::kProjectSchemaMajor) +
                    ") — refusing to open a project file from a newer/unknown format "
                    "rather than silently mis-read it. Open it with a matching backend "
                    "version.";
                std::fprintf(stderr, "[xinsp2] %s\n", last_open_error_.c_str());
                yyjson_doc_free(doc);
                return false;
            }
            // recognized current-or-older major → load normally (no log).
        } else {
            std::fprintf(stderr,
                "[xinsp2] project.json has no \"schema\" field — treating as a "
                "legacy (pre-schema) project; the next save will stamp \"%s\".\n",
                xi::project::kProjectSchema);
        }
        if (yyjson_val* pd = yyjson_obj_get(root, "plugin_dirs"); pd && yyjson_is_arr(pd)) {
            size_t _pi, _pn; yyjson_val* it;
            yyjson_arr_foreach(pd, _pi, _pn, it)
                if (yyjson_is_str(it) && yyjson_get_str(it)) proj_plugin_dirs.push_back(yyjson_get_str(it));
        }
        if (yyjson_val* pl = yyjson_obj_get(root, "plugins"); pl && yyjson_is_obj(pl)) {
            size_t _pi, _pn; yyjson_val *k, *v;
            yyjson_obj_foreach(pl, _pi, _pn, k, v) {
                if (!yyjson_is_obj(v)) continue;
                const char* label = yyjson_get_str(k);
                yyjson_val* pathv = yyjson_obj_get(v, "path");
                if (label && pathv && yyjson_is_str(pathv) && yyjson_get_str(pathv)) {
                    bool compile = proj_plugin_dirs_compile;   // project default
                    if (yyjson_val* cv = yyjson_obj_get(v, "compile"); cv && yyjson_is_bool(cv))
                        compile = yyjson_get_bool(cv);          // per-plugin override
                    proj_plugin_refs.push_back({label, yyjson_get_str(pathv), compile});
                }
            }
        }
        // H2: collect the top-level `instances` array (shape: [{name, plugin}]).
        // We DON'T materialize from it — that stays the instances/<name>/
        // instance.json job below — but we remember the declared names so a
        // phantom entry (declared here, no backing dir) gets a loud warning
        // instead of silently doing nothing (found by qa_multi_graph).
        if (yyjson_val* insts = yyjson_obj_get(root, "instances"); insts && yyjson_is_arr(insts)) {
            size_t _ii, _in; yyjson_val* it;
            yyjson_arr_foreach(insts, _ii, _in, it) {
                if (!yyjson_is_obj(it)) continue;
                const char* nm = nullptr; const char* pl = nullptr;
                if (yyjson_val* nv = yyjson_obj_get(it, "name");   nv && yyjson_is_str(nv)) nm = yyjson_get_str(nv);
                if (yyjson_val* pv = yyjson_obj_get(it, "plugin"); pv && yyjson_is_str(pv)) pl = yyjson_get_str(pv);
                if (nm && *nm) proj_declared_instances.emplace_back(nm, pl ? pl : "");
            }
        }
        // (trigger_policy removed in the ABI-v6 dispatch cleanup — multi-cam
        // sync is now a gathering plugin, so the bus does no correlation. A
        // legacy trigger_policy block in project.json is simply ignored.)
        // runtime block — operational knobs (also live-settable). process_priority
        // applied on open; timer_fps seeds the live timer rate.
        if (yyjson_val* rt = yyjson_obj_get(root, "runtime"); rt && yyjson_is_obj(rt)) {
            if (yyjson_val* k = yyjson_obj_get(rt, "process_priority"); k && yyjson_is_str(k) && yyjson_get_str(k))
                project_.runtime_priority = yyjson_get_str(k);
            if (yyjson_val* k = yyjson_obj_get(rt, "timer_fps"); k && yyjson_is_num(k))
                project_.runtime_timer_fps = (int)yyjson_get_num(k);
        }
        // parallelism block.
        if (yyjson_val* par = yyjson_obj_get(root, "parallelism");
            par && yyjson_is_obj(par)) {
            if (yyjson_val* k = yyjson_obj_get(par, "dispatch_threads");
                k && yyjson_is_num(k)) {
                int n = (int)yyjson_get_num(k);
                if (n < 1) n = 1;
                if (n > 32) n = 32;  // sanity cap
                project_.dispatch_threads = n;
            }
            if (yyjson_val* k = yyjson_obj_get(par, "queue_depth");
                k && yyjson_is_num(k)) {
                int n = (int)yyjson_get_num(k);
                // 0 is now valid — the RENDEZVOUS (synchronous-handoff) rung (only
                // with overflow:block; validated below once overflow is also known).
                if (n < 0)     n = 0;
                if (n > 10000) n = 10000;
                project_.queue_depth = n;
            }
            // max_inflight: ceiling on concurrent detached one-shot inspects (B1).
            // <=0/absent keeps the default (64); sanity-capped like queue_depth.
            if (yyjson_val* k = yyjson_obj_get(par, "max_inflight");
                k && yyjson_is_num(k)) {
                int n = (int)yyjson_get_num(k);
                if (n < 1)     n = 64;      // 0/absent/negative → default, NOT unlimited
                if (n > 10000) n = 10000;
                project_.max_inflight = n;
            }
            if (yyjson_val* k = yyjson_obj_get(par, "overflow");
                k && yyjson_is_str(k) && yyjson_get_str(k)) {
                std::string s = yyjson_get_str(k);
                // "block" is supported again (safe interruptible back-pressure form:
                // the producer parks on a slot and a stop/teardown wakes it to DROP,
                // so it can't hang teardown). OPT-IN only, and ONLY for a back-
                // pressure-TOLERANT source (dedicated timer/emit or file/disk batch
                // thread). Do NOT point block at a self-feeding worker lane (parked
                // worker can't drain its own lane → deadlock until stop) or a real-
                // time camera-callback thread (stalls acquisition). Default stays
                // drop_oldest (the line wants the freshest frame).
                if (s == "drop_oldest" || s == "drop_newest" || s == "block") {
                    project_.overflow = s;
                } else {
                    std::fprintf(stderr,
                        "[xinsp2] project.json parallelism.overflow "
                        "unknown value '%s' — using drop_oldest\n",
                        s.c_str());
                }
            }
            if (yyjson_val* k = yyjson_obj_get(par, "result_order");
                k && yyjson_is_str(k) && yyjson_get_str(k)) {
                std::string s = yyjson_get_str(k);
                if (s == "completion" || s == "arrival") {
                    project_.result_order = s;
                } else {
                    std::fprintf(stderr,
                        "[xinsp2] project.json parallelism.result_order "
                        "unknown value '%s' — using completion\n",
                        s.c_str());
                }
            }
            // depth=0 (rendezvous) is ONLY valid with overflow:block. With a drop
            // policy it has no slot to hold the event and no rendezvous wait, so it
            // would degenerate to "drop unless a worker is idle this instant" AND
            // could hit front() on an empty queue. Validate now that BOTH fields are
            // known (either may have parsed first): clamp back to 1 + warn loudly.
            if (project_.queue_depth == 0 && project_.overflow != "block") {
                std::fprintf(stderr,
                    "[xinsp2] project.json parallelism.queue_depth:0 (rendezvous) "
                    "requires overflow:block — clamping depth to 1\n");
                project_.queue_depth = 1;
            }
            // Advisory: depth-0 (rendezvous) is strict serial by definition, so the
            // dispatch pool CLAMPS it to a single worker at spawn (RB2, doc 25) — with
            // >1 the release-on-take path would otherwise run inspections concurrently
            // and break the 1-in-flight guarantee. The config value is left as-set; the
            // runtime just honours the rendezvous semantics. For multi-threaded
            // admission use the plugin-semaphore path over a NORMAL lane (doc 24 §4).
            if (project_.queue_depth == 0 && project_.dispatch_threads > 1) {
                std::fprintf(stderr,
                    "[xinsp2] project.json parallelism.queue_depth:0 (rendezvous) runs a "
                    "SINGLE worker — dispatch_threads=%d is clamped to 1 at spawn "
                    "(rendezvous is strict serial; use the plugin-semaphore path for "
                    "multi-threaded admission)\n", project_.dispatch_threads);
            }
            // parallelism.groups + default_group (optional; empty = legacy pool)
            if (yyjson_val* arr = yyjson_obj_get(par, "groups"); arr && yyjson_is_arr(arr)) {
                auto warn = [&](const std::string& who, const std::string& msg) {
                    last_open_warnings_.push_back({who, "", msg});
                    std::fprintf(stderr, "[xinsp2] parallelism.groups: %s — %s\n", who.c_str(), msg.c_str());
                };
                size_t _gi, _gn; yyjson_val* g;
                yyjson_arr_foreach(arr, _gi, _gn, g) {
                    if (!yyjson_is_obj(g)) continue;
                    ProjectInfo::DispatchGroup grp;
                    if (yyjson_val* k = yyjson_obj_get(g, "name"); k && yyjson_is_str(k) && yyjson_get_str(k)) grp.name = yyjson_get_str(k);
                    if (grp.name.empty()) { warn("(unnamed)", "group missing 'name' — skipped"); continue; }
                    if (project_.find_group(grp.name)) { warn(grp.name, "duplicate group name — skipped"); continue; }  // #7
                    if (yyjson_val* k = yyjson_obj_get(g, "max_parallel"); k && yyjson_is_num(k))
                        grp.max_parallel = std::min(32, std::max(1, (int)yyjson_get_num(k)));   // #4 clamp [1,32]
                    if (yyjson_val* k = yyjson_obj_get(g, "thread_priority"); k && yyjson_is_str(k) && yyjson_get_str(k)) {
                        grp.thread_priority = yyjson_get_str(k);
                        if (grp.thread_priority != "high" && grp.thread_priority != "normal" && grp.thread_priority != "low") {
                            warn(grp.name, "unknown thread_priority '" + grp.thread_priority + "' — using normal");
                            grp.thread_priority = "normal";
                        }
                    }
                    if (yyjson_val* k = yyjson_obj_get(g, "queue_depth"); k && yyjson_is_num(k))
                        grp.queue_depth = std::min(10000, std::max(0, (int)yyjson_get_num(k)));  // 0 = rendezvous (block only)
                    if (yyjson_val* k = yyjson_obj_get(g, "overflow"); k && yyjson_is_str(k) && yyjson_get_str(k)) {
                        grp.overflow = yyjson_get_str(k);
                        // "block" supported again (interruptible back-pressure; opt-in;
                        // back-pressure-tolerant sources ONLY — deadlock risk on a self-
                        // feeding worker lane or a camera-callback thread; see project-
                        // level note above).
                        if (grp.overflow != "drop_oldest" && grp.overflow != "drop_newest" && grp.overflow != "block") {
                            warn(grp.name, "unknown overflow '" + grp.overflow + "' — using drop_oldest");
                            grp.overflow = "drop_oldest";
                        }
                    }
                    if (yyjson_val* k = yyjson_obj_get(g, "result_order"); k && yyjson_is_str(k) && yyjson_get_str(k)) {
                        grp.result_order = yyjson_get_str(k);
                        if (grp.result_order != "completion" && grp.result_order != "arrival") {
                            warn(grp.name, "unknown result_order '" + grp.result_order + "' — using completion");
                            grp.result_order = "completion";
                        }
                    }
                    if (yyjson_val* k = yyjson_obj_get(g, "min_interval_ms"); k && yyjson_is_num(k))
                        grp.min_interval_ms = std::min(3600000, std::max(0, (int)yyjson_get_num(k)));
                    // cpu_affinity: flat [0,1,..] = one shared mask; nested
                    // [[..],[..]] = per-worker masks. Empty/invalid → unbound.
                    if (yyjson_val* k = yyjson_obj_get(g, "cpu_affinity"); k && yyjson_is_arr(k)) {
                        auto parse_set = [&](yyjson_val* arr) {
                            std::vector<int> s;
                            size_t _ei, _en; yyjson_val* e;
                            yyjson_arr_foreach(arr, _ei, _en, e) {
                                if (!yyjson_is_num(e)) continue;
                                int c = (int)yyjson_get_num(e);
                                if (c >= 0 && c < 1024) s.push_back(c);
                                else warn(grp.name, "cpu_affinity core " + std::to_string(c) + " out of range — ignored");
                            }
                            return s;
                        };
                        yyjson_val* first = yyjson_arr_get(k, 0);
                        if (first && yyjson_is_arr(first)) {          // nested: per-worker
                            size_t _ri, _rn; yyjson_val* row;
                            yyjson_arr_foreach(k, _ri, _rn, row)
                                if (yyjson_is_arr(row)) { auto s = parse_set(row); if (!s.empty()) grp.cpu_affinity.push_back(std::move(s)); }
                        } else {                                       // flat: one shared mask
                            auto s = parse_set(k);
                            if (!s.empty()) grp.cpu_affinity.push_back(std::move(s));
                        }
                    }
                    // depth=0 (rendezvous) requires overflow:block (both fields now
                    // parsed for this group, in either order) — else clamp to 1 + warn.
                    // Keeps the depth==0 dispatch branch's "never front()-on-empty"
                    // invariant true by construction (see enqueue_to_lane_).
                    if (grp.queue_depth == 0 && grp.overflow != "block") {
                        warn(grp.name, "queue_depth:0 (rendezvous) requires overflow:block — clamping depth to 1");
                        grp.queue_depth = 1;
                    }
                    // Advisory (honoured, not clamped): rendezvous is strict 1-in-flight,
                    // so the pool clamps it to a single worker at spawn (RB2, doc 25).
                    if (grp.queue_depth == 0 && grp.max_parallel > 1)
                        warn(grp.name, "queue_depth:0 (rendezvous) runs a SINGLE worker — max_parallel is clamped to 1 at spawn (strict serial; use the plugin-semaphore path for multi-threaded admission)");
                    project_.groups.push_back(std::move(grp));
                }
                if (yyjson_val* k = yyjson_obj_get(par, "default_group"); k && yyjson_is_str(k) && yyjson_get_str(k))
                    project_.default_group = yyjson_get_str(k);
                if (project_.default_group.empty() && !project_.groups.empty())
                    project_.default_group = project_.groups.front().name;
                if (!project_.default_group.empty() && !project_.find_group(project_.default_group))  // #6
                    warn(project_.default_group, "default_group names no declared group — falling back to '" +
                         (project_.groups.empty() ? std::string("(none)") : project_.groups.front().name) + "'");
            }
        }
        yyjson_doc_free(doc);
    } else {
        yyjson_doc_free(doc);
        project_json_malformed = true;
    }
    // Compile project-local plugins BEFORE instances are loaded — the
    // instance loop below resolves plugin name → loaded DLL, and we
    // want project plugins to win over global ones with the same name.
    // (last_open_warnings_ was reset at the top of open_project so group-parse
    // warnings, compile failures, and bad instances all accumulate together.)
    // Record the malformed verdict for the lifetime of this open so the
    // save path can refuse the destructive rebuild (BUG#4). A clean open
    // sets this back to false (local default), so the flag never sticks
    // across a later good open.
    project_json_malformed_ = project_json_malformed;
    if (project_json_malformed) {
        last_open_warnings_.push_back({"", "",
            "project.json is not valid JSON - opened READ-ONLY/degraded; "
            "saves are blocked until it is fixed (check for a syntax error "
            "or truncation). Original bytes preserved as project.json.corrupt-<ts>."});
        std::fprintf(stderr, "[xinsp2] project.json malformed - opened degraded; saves blocked\n");
        // Preserve the recoverable original bytes BEFORE any later save can
        // overwrite project.json. Copy verbatim to a timestamped sibling so
        // the user (or a tool) can recover the extension-owned keys. Portable
        // timestamp via xi::wall_ms() (no platform-specific time call). Done
        // once here at open — the save path only refuses, it never copies.
        std::error_code qec;
        auto corrupt = pj;
        corrupt += ".corrupt-" + std::to_string(xi::wall_ms());
        std::filesystem::copy_file(pj, corrupt,
            std::filesystem::copy_options::overwrite_existing, qec);
        if (qec) {
            std::fprintf(stderr,
                "[xinsp2] project.json: could not write quarantine copy %s (%s)\n",
                corrupt.string().c_str(), qec.message().c_str());
        } else {
            std::fprintf(stderr, "[xinsp2] project.json: preserved original bytes as %s\n",
                         corrupt.string().c_str());
        }
    }
    // Declarative plugin model: EVERY plugin (local + external) comes from the
    // project.json `plugins` declarations, resolved against `plugin_dirs`. There
    // is NO auto-discovery of <project>/plugins/* — a folder there does nothing
    // until it's listed in `plugins`. `plugin_dirs` falls back to ["./plugins"]
    // only when unset; set it and you get exactly your roots (re-add "./plugins"
    // yourself if you still want it). Each `plugins` entry's `compile` decides
    // whether its resolved folder is cl.exe-compiled / treated as a trusted
    // project plugin (recompile/rebuild-able) or just registered.
    // Persist the declarations as-written (pre-fallback) so save_project_locked
    // round-trips them without baking in the implicit ./plugins.
    project_.plugin_dirs = proj_plugin_dirs;
    project_.plugins     = proj_plugin_refs;
    if (proj_plugin_dirs.empty()) proj_plugin_dirs.push_back("./plugins");
    resolve_external_project_plugins_locked_(folder, proj_plugin_dirs, proj_plugin_refs);

    // Scan instances/ subdirectories. A broken instance.json or a
    // factory that throws must NOT abort the whole project load —
    // record the failure in last_open_warnings_ and move on. The
    // user can read it via cmd:open_project_warnings and decide
    // whether to fix or delete the bad instance folder.
    // (last_open_warnings_ already cleared above before plugin compile.)
    auto inst_dir = std::filesystem::path(folder) / "instances";
    bool warned_iso_deprecated_ = false;
    if (std::filesystem::exists(inst_dir)) {
        for (auto& entry : std::filesystem::directory_iterator(inst_dir)) {
            if (!entry.is_directory()) continue;
            std::string inst_name = entry.path().filename().string();
            try {
            auto ij = entry.path() / "instance.json";
            if (!std::filesystem::exists(ij)) {
                last_open_warnings_.push_back({inst_name, "", "missing instance.json"});
                std::fprintf(stderr, "[xinsp2] skip instance '%s': missing instance.json\n",
                             inst_name.c_str());
                continue;
            }
            std::ifstream ijf(ij.string());
            std::stringstream iss;
            iss << ijf.rdbuf();
            std::string ic = iss.str();
            auto plugin = extract_string(ic, "plugin");
            if (!plugin) {
                last_open_warnings_.push_back({inst_name, "", "instance.json missing 'plugin' field (or unparseable)"});
                std::fprintf(stderr, "[xinsp2] skip instance '%s': no plugin field\n",
                             inst_name.c_str());
                continue;
            }

            InstanceInfo ii;
            ii.name = inst_name;
            ii.plugin_name = *plugin;
            ii.folder_path = entry.path().string();
            if (auto g = extract_string(ic, "group")) ii.group = *g;   // dispatch group for this source's triggers
            // Optional per-instance concurrency cap (reentrant plugins only;
            // 0/absent = unlimited). Parse JSON for the numeric field.
            if (yyjson_doc* idoc = yyjson_read(ic.c_str(), ic.size(), 0)) {
                yyjson_val* iroot = yyjson_doc_get_root(idoc);
                if (yyjson_val* k = iroot ? yyjson_obj_get(iroot, "max_concurrency") : nullptr;
                    k && yyjson_is_num(k) && yyjson_get_num(k) > 0) {
                    ii.max_concurrency = (int)yyjson_get_num(k);
                }
                yyjson_doc_free(idoc);
            }
            // Auto-load the plugin if not yet loaded
            auto pit = plugins_.find(*plugin);
            if (pit != plugins_.end() && !pit->second.c_factory) {
                // Plugin discovered but not loaded — load it now
                auto& pi2 = pit->second;
                auto dll_path = std::filesystem::path(pi2.folder_path) / pi2.dll_name;
                if (std::filesystem::exists(dll_path)) {
                    pi2.handle = load_plugin_dll_(dll_path.string());
                    if (pi2.handle) {
                        // P0-D3: ABI compatibility check was missing
                        // on this code path. A stale plugin DLL built
                        // against a future ABI loaded silently and
                        // got called with the new ABI signatures —
                        // memory corruption risk. Mirror the load_plugin
                        // path's check.
                        std::string err;
                        if (!plugin_abi_compatible(pi2.handle, *plugin, pi2.json_fallback, &err)) {
                            FreeLibrary(pi2.handle);
                            pi2.handle = nullptr;
                            last_open_warnings_.push_back(
                                {inst_name, *plugin, "plugin ABI mismatch: " + err});
                            std::fprintf(stderr,
                                "[xinsp2] skip instance '%s': %s\n",
                                inst_name.c_str(), err.c_str());
                            continue;
                        }
                        // LV2-style capability handshake — same refuse-with-reason
                        // surfacing as the ABI gate just above (skip the instance,
                        // record the reason in last_open_warnings_).
                        if (!plugin_caps_compatible(pi2, &default_host_api(), *plugin, &err)) {
                            FreeLibrary(pi2.handle);
                            pi2.handle = nullptr;
                            last_open_warnings_.push_back(
                                {inst_name, *plugin, "plugin capability gate: " + err});
                            std::fprintf(stderr,
                                "[xinsp2] skip instance '%s': %s\n",
                                inst_name.c_str(), err.c_str());
                            continue;
                        }
                        pi2.c_factory = reinterpret_cast<PluginInfo::CFactoryFn>(
                            GetProcAddress(pi2.handle, pi2.factory_symbol.c_str()));
                        // P0-D3 (cont.): if the factory symbol doesn't
                        // resolve, the DLL is loaded but unusable.
                        // Previously left in place with handle set
                        // and factory=null; subsequent open_project
                        // attempts found a stale entry. FreeLibrary
                        // and clear handle so the entry stays in a
                        // clean "not loaded" state.
                        if (!pi2.c_factory) {
                            FreeLibrary(pi2.handle);
                            pi2.handle = nullptr;
                            last_open_warnings_.push_back(
                                {inst_name, *plugin,
                                 "plugin DLL has no factory symbol "
                                 + pi2.factory_symbol});
                            std::fprintf(stderr,
                                "[xinsp2] skip instance '%s': no factory '%s'\n",
                                inst_name.c_str(),
                                pi2.factory_symbol.c_str());
                            continue;
                        }
                        stamp_loaded_dll_(pi2, dll_path.string());   // change-gate for reload_changed
                    }
                }
            }
            if (pit != plugins_.end()) {
                auto& pi = pit->second;
                bool created = false;
                // Same registration as create_instance — needed for project-load too.
                InstanceFolderRegistry::instance().set(ii.name, ii.folder_path);
                // V3 precedence: an explicit project instance of an autoload lib
                // plugin displaces the boot machine provider so this instance
                // registers its capabilities cleanly (no ETAKEN). Reinstated on
                // close_project. (No-op unless *plugin is machine-provided.)
                evict_machine_provider_locked_(*plugin);

                // Every instance runs in-process: plugins are loaded
                // into the backend and called directly (zero-copy via
                // pointers into the host ImagePool).
                //
                // Architecture pivot (2026-05): process isolation + SHM
                // were removed in favour of a single in-process compute
                // core under a frontend supervisor. A dead plugin means
                // a dead pipeline regardless of isolation, so per-plugin
                // sandboxing bought nothing but the SHM / worker-process
                // complexity. `isolation:"process"` (and "in_process")
                // are accepted but ignored, with a one-time deprecation
                // warning, so old project.json files keep loading.
                if (auto iso = extract_string(ic, "isolation");
                    iso && *iso == "process" && !warned_iso_deprecated_) {
                    std::fprintf(stderr,
                        "[xinsp2] isolation:\"process\" is deprecated and ignored — "
                        "all plugins run in-process now (single compute core). "
                        "Remove the field from instance.json to silence this.\n");
                    warned_iso_deprecated_ = true;
                }

                // item 14: resolve the effective post-fault policy — instance.json
                // "on_fault" override if present, else the plugin default.
                ii.on_fault = parse_on_fault(extract_string(ic, "on_fault").value_or(""),
                                             pi.default_on_fault);
                if (!created && pi.c_factory) {
                    xi_host_api& host = default_host_api();
                    // ImagePoolOwnerScope tags the ctor's images and sweeps them
                    // automatically on a null return OR a throw (its dtor runs as
                    // the exception unwinds to the per-instance handler below) —
                    // the adapter never got built, so it can't sweep for us.
                    ImagePoolOwnerScope owner;
                    void* raw = owner.run_factory(
                        [&] { return pi.c_factory(&host, ii.name.c_str()); });
                    if (raw) {
                        auto adapter = std::make_shared<CAbiInstanceAdapter>(
                            ii.name, *plugin, pi.handle, raw, pi.reentrant, ii.max_concurrency, pi.is_sink);
                        // Hand the owner id to the adapter so subsequent process /
                        // exchange calls keep tagging into the same bucket.
                        adapter->adopt_owner_id(owner.release());
                        adapter->set_on_fault(ii.on_fault);             // item 14
                        adapter->arm_reinit(pi.c_factory, &host);        // enable in-place reinit
                        ii.instance = std::move(adapter);
                        created = true;
                    }
                }
                if (created && ii.instance) {
                    std::string cfg_val;
                    if (detail_find_key(ic, "config", cfg_val)) {
                        // FL r6 P2-3: validate instance.json.config against
                        // plugin.json.manifest.params before handing it to
                        // the plugin. Bad keys / out-of-range values still
                        // fall through to set_def() (which silently ignores
                        // unknown keys); the warnings here are how the user
                        // learns about a typo. Skipped if the plugin
                        // doesn't declare manifest.params (back-compat).
                        validate_config_against_manifest(
                            inst_name, *plugin, cfg_val,
                            pi.manifest_json, last_open_warnings_);
                        ii.instance->set_def(cfg_val);
                    }
                    InstanceRegistry::instance().add(ii.instance);
                }
                if (!created) {
                    // B-P1-2: factory failed → adapter wasn't created,
                    // but InstanceFolderRegistry::set ran a few lines
                    // up. Clear the folder-registry entry so a stale
                    // reference doesn't outlive this open_project.
                    InstanceFolderRegistry::instance().clear(inst_name);
                    last_open_warnings_.push_back(
                        {inst_name, *plugin, "factory returned null"});
                    std::fprintf(stderr,
                        "[xinsp2] skip instance '%s' (%s): factory returned null\n",
                        inst_name.c_str(), plugin->c_str());
                } else {
                    // Only persist the InstanceInfo in project_.instances
                    // when we actually have a live adapter. Skip-bad
                    // entries shouldn't become "phantom" entries that
                    // close_project then iterates over.
                    project_.instances[ii.name] = std::move(ii);
                }
            } else {
                // B-P1-2: same gap as the factory-null branch above —
                // InstanceFolderRegistry::set wasn't called yet here
                // (we only set it after pit was found), but log+continue
                // shape stays consistent.
                last_open_warnings_.push_back(
                    {inst_name, *plugin, "plugin not loaded / not found"});
                std::fprintf(stderr,
                    "[xinsp2] skip instance '%s': plugin '%s' not loaded\n",
                    inst_name.c_str(), plugin->c_str());
            }
            } catch (const std::exception& e) {
                // B-P1-1: catch ran AFTER InstanceFolderRegistry::set
                // and (potentially) InstanceRegistry::add inside the
                // try block, but didn't undo either. Stale entries
                // leaked until the NEXT open_project's bulk clear.
                // Symmetric cleanup here:
                InstanceFolderRegistry::instance().clear(inst_name);
                InstanceRegistry::instance().remove(inst_name);
                last_open_warnings_.push_back(
                    {inst_name, "", std::string("exception: ") + e.what()});
                std::fprintf(stderr,
                    "[xinsp2] skip instance '%s': %s\n",
                    inst_name.c_str(), e.what());
            } catch (...) {
                InstanceFolderRegistry::instance().clear(inst_name);
                InstanceRegistry::instance().remove(inst_name);
                last_open_warnings_.push_back(
                    {inst_name, "", "unknown exception during load"});
                std::fprintf(stderr,
                    "[xinsp2] skip instance '%s': unknown exception\n",
                    inst_name.c_str());
            }
        }
    }

    // H2: cross-check the project.json `instances` array against what actually
    // materialized on disk. An instance ONLY exists via instances/<name>/
    // instance.json (scanned above); a project.json entry with no backing dir
    // is inert and used to vanish with NO signal — a project.json-only source
    // (e.g. an `expose` sink) that silently didn't exist (qa_multi_graph). Warn
    // loudly, naming each phantom, so the misconfig surfaces via
    // cmd:open_project_warnings instead of manifesting as a mystery-missing
    // instance far downstream. (A declared entry WHOSE dir exists but failed to
    // load was already warned by the scan loop; we key off the missing backing
    // file so we don't double-report it.)
    for (auto& [nm, pl] : proj_declared_instances) {
        auto backing = std::filesystem::path(folder) / "instances" / nm / "instance.json";
        if (std::filesystem::exists(backing)) continue;          // real dir handled above
        std::string msg =
            "declared in project.json 'instances' but has no instances/" + nm +
            "/instance.json — this entry is INERT (instances materialize only "
            "from their instance.json). Create the folder or remove the "
            "project.json entry.";
        last_open_warnings_.push_back({nm, pl, msg});
        std::fprintf(stderr,
            "[xinsp2] project.json declares instance '%s' (plugin '%s') with no "
            "backing instances/%s/instance.json — INERT, not created.\n",
            nm.c_str(), pl.c_str(), nm.c_str());
    }
    return true;
}

} // namespace xi
