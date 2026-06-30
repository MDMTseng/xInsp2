#pragma once
//
// xi_working_copy.hpp — filesystem mechanics for transactional working-copy
// edits (the <project>/.xinsp_work scratch model).
//
// Extracted from xi_plugin_manager.hpp: these are pure, stateless filesystem
// operations (seed / mirror / exclusion / gitignore) — the "how to copy a
// project tree transactionally" mechanic. The PluginManager keeps the stateful
// transactional methods (open/commit/discard, which touch canonical_path_ and
// reopen the project) and calls into these.
//
// TODO(linux): std::filesystem only — already portable.
//
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace xi {
namespace wc {

// The scratch dir holding the working copy, under the canonical project root.
inline constexpr const char* kWorkingCopyDir = ".xinsp_work";

// Commit-in-progress journal marker. Written at the canonical root before a
// commit's mirror starts and removed after it completes. If it survives (a
// crash/power-loss mid-commit left the canonical tree torn), the next
// open_project rolls the commit forward from the intact scratch.
inline constexpr const char* kCommitMarker = ".xinsp_commit_pending";

// Advisory single-writer stamp (xi_owner_lock.hpp). Named here so the seed/commit
// exclusion below can skip it — it's per-process bookkeeping at the canonical root,
// not project content, so it must never ride into the scratch or back onto canonical.
inline constexpr const char* kOwnerFile = ".xinsp_owner";

// True if a project-relative path should be skipped when seeding/mirroring the
// working copy. PRECISE on purpose: matching these names at ANY depth (the old
// behaviour) silently dropped a USER directory that happened to share a name —
// e.g. an asset/data folder named "build" under instances/<x>/ — from BOTH the
// seed AND the commit prune, i.e. silent data loss the user never sees.
//   - .xinsp_work / .git / commit-marker: only ever exist at the project ROOT, so
//     anchor them there.
//   - "build": the only in-tree build output is a CMake plugin's plugins/<name>/build
//     (regenerated on rebuild — no point copying, and committing it back would
//     clobber the canonical build). So exclude "build" ONLY under plugins/.
inline bool is_excluded(const std::filesystem::path& rel) {
    if (rel.empty()) return false;
    const std::string first = rel.begin()->string();
    if (first == kWorkingCopyDir || first == ".git" || first == kCommitMarker ||
        first == kOwnerFile)
        return true;
    if (first == "plugins") {
        for (const auto& part : rel)
            if (part.string() == "build") return true;
    }
    return false;
}

// Recursively copy `src` -> `dst`, skipping is_excluded paths. Used to seed a
// fresh working copy from the canonical project.
// Returns false if any directory-create or file-copy failed: a swallowed error
// here (disk full, read-only dest, a locked file) would leave a torn tree while
// the caller reports success — see commit_working_copy.
inline bool copy_tree_excluding(const std::filesystem::path& src,
                                const std::filesystem::path& dst) {
    std::error_code ec;
    bool ok = true;
    std::filesystem::create_directories(dst, ec);
    if (ec) ok = false;
    for (auto it = std::filesystem::recursive_directory_iterator(
             src, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        auto rel = std::filesystem::relative(it->path(), src, ec);
        if (ec || rel.empty()) continue;
        if (is_excluded(rel)) { if (it->is_directory()) it.disable_recursion_pending(); continue; }
        auto target = dst / rel;
        std::error_code op;
        if (it->is_directory()) {
            std::filesystem::create_directories(target, op);
        } else {
            std::filesystem::create_directories(target.parent_path(), op);
            if (op) { ok = false; continue; }
            std::filesystem::copy_file(it->path(), target,
                std::filesystem::copy_options::overwrite_existing, op);
        }
        if (op) ok = false;
    }
    // A failure during iteration itself (not skip_permission_denied) is also a
    // torn copy — surface it.
    if (ec) ok = false;
    return ok;
}

// Seed a FRESH working-copy scratch at `scratch` from the canonical project
// `canon`. Returns true only if the scratch is a COMPLETE copy that is safe to
// present to the user as authoritative.
//
// THE INVARIANT THIS GUARDS: a torn seed must never become a committable scratch.
// copy_tree_excluding can leave a half-populated tree on a disk error (full disk,
// locked / permission-denied file) that still happens to carry project.json. If
// the caller accepted that as authoritative, the user would edit it and the
// eventual commit's mirror_tree would PRUNE the canonical files that merely failed
// to copy in — silent data loss. So on a failed copy we tear the partial scratch
// back DOWN (remove_all) and return false, ensuring nothing — not the immediate
// open, not a later crash-resume / resume-existing path — can adopt it as a
// working copy. Mirrors how commit_working_copy honours mirror_tree's bool.
inline bool seed_working_copy(const std::filesystem::path& canon,
                              const std::filesystem::path& scratch) {
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);   // clear any partial seed first
    if (!copy_tree_excluding(canon, scratch)) {
        std::filesystem::remove_all(scratch, ec);   // never leave a torn, committable scratch
        return false;
    }
    return true;
}

// Mirror `src` (working copy) onto `dst` (canonical): copy/overwrite every
// file, then delete files/dirs in `dst` that aren't in `src` — so removals
// (e.g. a deleted instance) propagate. Excluded paths are left untouched on
// both sides (the canonical .git stays; build/ is regenerated). Idempotent, so
// a crash-interrupted commit can be rolled forward by re-running it.
// Returns false if the add/overwrite copy or any prune-removal failed — the
// caller must then leave the commit marker in place so the next open rolls the
// (still-intact) scratch forward, and report the failure rather than success.
inline bool mirror_tree(const std::filesystem::path& src,
                        const std::filesystem::path& dst) {
    std::error_code ec;
    bool ok = copy_tree_excluding(src, dst);   // adds + overwrites
    // Prune: remove dst entries with no src counterpart.
    std::vector<std::filesystem::path> to_remove;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dst, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        auto rel = std::filesystem::relative(it->path(), dst, ec);
        if (ec || rel.empty()) continue;
        if (is_excluded(rel)) { if (it->is_directory()) it.disable_recursion_pending(); continue; }
        if (!std::filesystem::exists(src / rel)) to_remove.push_back(it->path());
    }
    for (auto& p : to_remove) {
        std::error_code rm;
        std::filesystem::remove_all(p, rm);
        if (rm) ok = false;
    }
    return ok;
}

// Append `line` to <dir>/.gitignore if not already present (so the scratch dir
// isn't accidentally committed). Best-effort; ignores I/O errors.
inline void ensure_gitignore(const std::filesystem::path& dir,
                             const std::string& line) {
    auto gi = dir / ".gitignore";
    std::string content;
    { std::ifstream f(gi); std::stringstream ss; ss << f.rdbuf(); content = ss.str(); }
    if (content.find(line) != std::string::npos) return;
    std::ofstream f(gi, std::ios::app);
    if (!f) return;
    if (!content.empty() && content.back() != '\n') f << "\n";
    f << line << "\n";
}

} // namespace wc
} // namespace xi
