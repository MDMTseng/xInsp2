import * as path from 'path';

// One discovered plugin as the backend reports it.
export interface PluginInfo {
    name: string;
    description: string;
    folder: string;
    has_ui: boolean;
    loaded: boolean;
    // origin: "project" → built from <project>/plugins/<name>/, "global" → from a plugin folder on disk.
    // source_dir is set only when origin === "project".
    origin?: 'project' | 'global';
    source_dir?: string;
    // true = `build: cmake` plugin (owns its CMakeLists; rebuilt via rebuild_plugins,
    // not cl.exe). Drives the per-item "Rebuild this plugin" action.
    prebuilt?: boolean;
    // Free-form schema block from plugin.json's `manifest` (params / inputs /
    // outputs / exchange). The backend embeds it verbatim in list_plugins.
    manifest?: any;
}

// Plain in-memory cache of the last-known plugin set. Was a TreeDataProvider for
// the (removed) "Plugins" view; that view is gone — the Plugin Browser webview is
// now the single plugin-management surface, so this is just the data it reads.
// Kept as the one home for: the discovered plugin list, per-plugin instance use
// counts, and which scan folders the user may remove.
export class PluginRegistry {
    private plugins: PluginInfo[] = [];
    private instanceUseCounts = new Map<string, number>();
    // Folders the extension added (user-editable). The built-in plugins dir is NOT in here.
    private removableFolders: Set<string> = new Set();

    setRemovableFolders(folders: string[]) {
        this.removableFolders = new Set(folders.map(f => path.resolve(f).toLowerCase()));
    }

    isRemovable(folder: string): boolean {
        return this.removableFolders.has(path.resolve(folder).toLowerCase());
    }

    /** Read-only snapshot of last-known plugins for command callers + the browser. */
    listPlugins(): readonly PluginInfo[] { return this.plugins; }

    /** Instances using a given plugin in the open project (0 when unused). */
    uses(name: string): number { return this.instanceUseCounts.get(name) || 0; }

    update(plugins: PluginInfo[], instanceUseCounts?: Map<string, number>) {
        this.plugins = plugins.slice();
        if (instanceUseCounts) this.instanceUseCounts = instanceUseCounts;
    }
}
