//
// depprobe — loads a dependency DLL from its OWN folder and reports both the
// version it found and the path the module ACTUALLY loaded from. Two copies of
// this plugin (depprobe_a / depprobe_b) each ship a different version of the
// same-named dep.dll; loading both into the one backend process shows the
// same-base-name collision (see ../../README.md and docs/guides/adding-a-plugin.md).
//
// Identical to depprobe_b/plugin.cpp on purpose — only the plugin folder + the
// dep.dll bytes differ.
//
#include <windows.h>   // already pulled in transitively, but be explicit
#include <string>

// Directory this plugin DLL itself was loaded from (so we look for deps beside us).
static std::string self_dir() {
    HMODULE h = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&self_dir), &h);
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(h, buf, (DWORD)sizeof(buf));
    std::string p = buf;
    auto s = p.find_last_of("\\/");
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

static std::string fwd(std::string p) { for (auto& c : p) if (c == '\\') c = '/'; return p; }

// Pull a "<stem>.dll" token out of an arbitrary cmd string; default "dep.dll".
static std::string dll_from_cmd(const std::string& cmd) {
    auto pos = cmd.rfind(".dll");
    if (pos == std::string::npos) return "dep.dll";
    size_t start = pos;
    while (start > 0) {
        char c = cmd[start - 1];
        bool ident = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-';
        if (!ident) break;
        --start;
    }
    return cmd.substr(start, pos - start + 4);
}

class DepProbe : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    std::string get_def() const override { return "{}"; }
    bool set_def(const std::string&) override { return true; }
    xi::Record process(const xi::Record&) override { return xi::Record{}; }

    std::string exchange(const std::string& cmd) override {
        const std::string dir = self_dir();
        if (cmd.find("whereami") != std::string::npos)
            return "{\"dir\":\"" + fwd(dir) + "\"}";

        const std::string name = dll_from_cmd(cmd);
        // Two ways a plugin can pull in a dependency:
        //   byname  — load by BASE NAME, letting the loader search (incl. our own
        //             folder via DLL_LOAD_DIR). This is how a STATIC import is
        //             resolved, and where the classic "one module per base name
        //             per process" collision bites.
        //   fullpath — load by absolute path. The modern loader keys on the full
        //             path, so two same-named DLLs from different folders stay
        //             distinct.
        const bool byname = cmd.find("byname") != std::string::npos;
        const std::string requested = byname ? name : (dir + "\\" + name);
        // We deliberately do NOT FreeLibrary, so the module stays resident and a
        // second plugin asking for the same base name can hit this loaded copy.
        HMODULE h;
        if (byname) {
            // Resolve by BASE NAME with our own folder on the search path — this
            // mirrors how a static import ("I link foo.dll") gets resolved, and
            // is where the loader's per-base-name dedup kicks in.
            SetDllDirectoryA(dir.c_str());
            h = LoadLibraryA(name.c_str());
            SetDllDirectoryA(nullptr);
        } else {
            // Resolve by FULL PATH — the modern loader keys on the full path.
            h = LoadLibraryExA(requested.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
        }
        std::string loaded = "(failed)";
        int ver = -1;
        if (h) {
            char lb[MAX_PATH] = {0};
            GetModuleFileNameA(h, lb, (DWORD)sizeof(lb));
            loaded = fwd(lb);
            typedef int (*vfn)(void);
            vfn f = reinterpret_cast<vfn>(GetProcAddress(h, "dep_version"));
            if (f) ver = f();
        }
        return "{\"mode\":\"" + std::string(byname ? "byname" : "fullpath") +
               "\",\"requested\":\"" + fwd(requested) + "\",\"loaded\":\"" + loaded +
               "\",\"version\":" + std::to_string(ver) + "}";
    }
};

XI_PLUGIN_IMPL(DepProbe)
