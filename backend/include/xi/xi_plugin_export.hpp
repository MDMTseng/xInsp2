#pragma once
//
// xi_plugin_export.hpp — package a project plugin as a standalone deployable.
//
// Extracted from xi_plugin_manager.hpp: the export path is a self-contained
// build/packaging concern (recompile in Release, copy a deploy folder), not
// part of the live inspection runtime. It touches no PluginManager mutable
// state beyond what the caller resolves and hands in (the source dir, the
// PluginInfo, the compile env), so it lives here as a free function and the
// manager keeps only a thin lock-and-resolve wrapper.
//
// Pure file + compile work; the caller holds the PluginManager lock.
//
#include "xi_abi.h"              // XI_ABI_VERSION
#include "xi_atomic_io.hpp"      // atomic_write
#include "xi_cabi_adapter.hpp"   // PluginInfo
#include "xi_project_model.hpp"  // CompileEnv
#include "xi_script_compiler.hpp"// compile / CompileRequest / CompileMode / Diagnostic

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace xi {

// Result of export_project_plugin_impl. Surfaced to the ws layer (which reads
// ok / dest_dir / error / build_log). PluginManager aliases this as its nested
// ExportResult so existing callers compile unchanged.
struct PluginExportResult {
    bool                                ok = false;
    std::string                         dest_dir;   // <dest>/<name>
    std::string                         error;
    std::string                         build_log;
    std::vector<xi::script::Diagnostic> diagnostics;
};

// Export a project plugin as a standalone deployable folder. Steps:
//   1. Recompile in PluginExport mode (/O2 /Zi — Release with PDB).
//   2. Copy <plugin>.dll, <plugin>.pdb, plugin.json (auto-generated if
//      missing), and any ui/ subfolder into <dest_root>/<name>/.
// (Plugins are trusted — no baseline cert gate on export, removed 2026-06.)
//
// `src_dir` is the plugin's project origin folder, `pi` its loaded manifest
// info, `compile_env` the toolchain paths. The caller (PluginManager) has
// already verified this is a project plugin and holds its lock.
inline PluginExportResult export_project_plugin_impl(
    const std::string&            plugin_name,
    const std::filesystem::path&  src_dir,
    const PluginInfo&             pi,
    const CompileEnv&             compile_env,
    const std::string&            dest_root)
{
    PluginExportResult er;

    // Re-collect sources (mirror of compile_project_plugins_locked).
    std::vector<std::string> sources;
    auto walk = [&](const std::filesystem::path& dir) {
        for (auto& f : std::filesystem::directory_iterator(dir)) {
            if (!f.is_regular_file()) continue;
            auto ext = f.path().extension().string();
            if (ext == ".cpp" || ext == ".cc" || ext == ".cxx")
                sources.push_back(f.path().string());
        }
    };
    auto src_subdir = src_dir / "src";
    if (std::filesystem::exists(src_subdir)) walk(src_subdir);
    else                                     walk(src_dir);
    if (sources.empty()) { er.error = "no .cpp sources"; return er; }
    std::vector<std::string> includes;
    auto inc_dir = src_dir / "include";
    if (std::filesystem::exists(inc_dir)) includes.push_back(inc_dir.string());

    // Build into a separate export/ folder so the dev DLL isn't touched.
    auto export_build = src_dir / "export_build";
    xi::script::CompileRequest req;
    req.source_path    = sources.front();
    req.extra_sources.assign(sources.begin() + 1, sources.end());
    req.include_dirs   = includes;
    req.output_dir     = export_build.string();
    req.include_dir    = compile_env.include_dir;
    req.vcvars_path    = compile_env.vcvars_path;
    req.opencv_dir     = compile_env.opencv_dir;
    req.turbojpeg_root = compile_env.turbojpeg_root;
    req.ipp_root       = compile_env.ipp_root;
    req.mode           = xi::script::CompileMode::PluginExport;

    std::fprintf(stderr, "[xinsp2] export: compiling '%s' (Release)...\n",
                 plugin_name.c_str());
    auto cres = xi::script::compile(req);
    er.build_log   = cres.build_log;
    er.diagnostics = cres.diagnostics;
    if (!cres.ok) { er.error = "Release compile failed"; return er; }

    // Copy the deployable into dest_root/<plugin_name>/.
    auto dest = std::filesystem::path(dest_root) / plugin_name;
    std::error_code ec;
    std::filesystem::create_directories(dest, ec);

    // DLL — rename _v<n> versioning out so the deployed file is stable.
    auto dll_dest = dest / (plugin_name + ".dll");
    std::filesystem::copy_file(cres.dll_path, dll_dest,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) { er.error = "copy DLL: " + ec.message(); return er; }

    // PDB beside DLL — keep so end-user crashes can blame source line.
    auto pdb_src = std::filesystem::path(cres.dll_path)
                       .replace_extension(".pdb");
    if (std::filesystem::exists(pdb_src)) {
        std::filesystem::copy_file(pdb_src,
            dest / (plugin_name + ".pdb"),
            std::filesystem::copy_options::overwrite_existing, ec);
    }

    // plugin.json — synthesize from PluginInfo if there's no manifest in
    // the source folder. Generated form points dll/factory at the names
    // the export uses, so the deployed folder is self-contained. The
    // synthesized version stamps `abi_version` so a target backend
    // older than the plugin's compile-time ABI can detect the
    // mismatch on scan (matches the runtime plugin_abi_compatible
    // check via the DLL's xi_plugin_abi_version export).
    auto src_manifest = src_dir / "plugin.json";
    std::string manifest_text;
    if (std::filesystem::exists(src_manifest)) {
        std::ifstream mf(src_manifest.string());
        std::stringstream ms; ms << mf.rdbuf();
        manifest_text = ms.str();
    } else {
        manifest_text = "{\n";
        manifest_text += "  \"name\":        \"" + pi.name + "\",\n";
        manifest_text += "  \"description\": \"" + pi.description + "\",\n";
        manifest_text += "  \"dll\":         \"" + pi.name + ".dll\",\n";
        manifest_text += "  \"factory\":     \"" + pi.factory_symbol + "\",\n";
        manifest_text += "  \"has_ui\":      " + std::string(pi.has_ui ? "true" : "false") + ",\n";
        manifest_text += "  \"abi_version\": " + std::to_string(XI_ABI_VERSION) + "\n";
        manifest_text += "}\n";
    }
    xi::atomic_write(dest / "plugin.json", manifest_text);

    // ui/ — copy whole subtree if present.
    auto ui_src = src_dir / "ui";
    if (std::filesystem::exists(ui_src)) {
        std::filesystem::copy(ui_src, dest / "ui",
            std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing, ec);
    }

    er.ok       = true;
    er.dest_dir = dest.string();
    std::fprintf(stderr, "[xinsp2] exported '%s' to %s\n",
                 plugin_name.c_str(), er.dest_dir.c_str());
    return er;
}

} // namespace xi
