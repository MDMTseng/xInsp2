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

// True if a path component should be skipped when seeding/mirroring the working
// copy: the scratch dir itself, VCS metadata, and regenerated build output
// (recompiled on open, no point copying — and committing it back would clobber
// the canonical build with the scratch's).
inline bool is_excluded(const std::filesystem::path& rel) {
    for (const auto& part : rel) {
        std::string s = part.string();
        if (s == kWorkingCopyDir || s == ".git" || s == "build" ||
            s == kCommitMarker) return true;
    }
    return false;
}

// Recursively copy `src` -> `dst`, skipping is_excluded paths. Used to seed a
// fresh working copy from the canonical project.
inline void copy_tree_excluding(const std::filesystem::path& src,
                                const std::filesystem::path& dst) {
    std::error_code ec;
    std::filesystem::create_directories(dst, ec);
    for (auto it = std::filesystem::recursive_directory_iterator(
             src, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        auto rel = std::filesystem::relative(it->path(), src, ec);
        if (ec || rel.empty()) continue;
        if (is_excluded(rel)) { if (it->is_directory()) it.disable_recursion_pending(); continue; }
        auto target = dst / rel;
        if (it->is_directory()) {
            std::filesystem::create_directories(target, ec);
        } else {
            std::filesystem::create_directories(target.parent_path(), ec);
            std::filesystem::copy_file(it->path(), target,
                std::filesystem::copy_options::overwrite_existing, ec);
        }
    }
}

// Mirror `src` (working copy) onto `dst` (canonical): copy/overwrite every
// file, then delete files/dirs in `dst` that aren't in `src` — so removals
// (e.g. a deleted instance) propagate. Excluded paths are left untouched on
// both sides (the canonical .git stays; build/ is regenerated). Idempotent, so
// a crash-interrupted commit can be rolled forward by re-running it.
inline void mirror_tree(const std::filesystem::path& src,
                        const std::filesystem::path& dst) {
    std::error_code ec;
    copy_tree_excluding(src, dst);   // adds + overwrites
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
    for (auto& p : to_remove) std::filesystem::remove_all(p, ec);
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
