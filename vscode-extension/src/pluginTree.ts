import * as vscode from 'vscode';
import * as path from 'path';

// One discovered plugin as the backend reports it.
export interface PluginInfo {
    name: string;
    description: string;
    folder: string;
    has_ui: boolean;
    loaded: boolean;
    cert?: { present: boolean; valid?: boolean; baseline_version?: number; certified_at?: string };
}

type Node =
    | { kind: 'folder'; folder: string; plugins: PluginInfo[]; removable: boolean }
    | { kind: 'plugin'; info: PluginInfo }
    | { kind: 'empty'; label: string };

export class PluginTreeProvider implements vscode.TreeDataProvider<Node> {
    private _onDidChange = new vscode.EventEmitter<Node | void>();
    readonly onDidChangeTreeData = this._onDidChange.event;

    private plugins: PluginInfo[] = [];
    private instanceUseCounts = new Map<string, number>();
    // Folders the extension added (user-editable). The built-in plugins dir is NOT in here.
    private removableFolders: Set<string> = new Set();

    setRemovableFolders(folders: string[]) {
        this.removableFolders = new Set(folders.map(f => path.resolve(f).toLowerCase()));
    }

    update(plugins: PluginInfo[], instanceUseCounts?: Map<string, number>) {
        this.plugins = plugins.slice();
        if (instanceUseCounts) this.instanceUseCounts = instanceUseCounts;
        this._onDidChange.fire();
    }

    getTreeItem(n: Node): vscode.TreeItem {
        if (n.kind === 'empty') {
            const ti = new vscode.TreeItem(n.label);
            ti.contextValue = 'empty';
            ti.iconPath = new vscode.ThemeIcon('info');
            return ti;
        }
        if (n.kind === 'folder') {
            const parent = path.basename(n.folder) || n.folder;
            const ti = new vscode.TreeItem(parent, vscode.TreeItemCollapsibleState.Expanded);
            ti.description = `${n.plugins.length} plugin${n.plugins.length === 1 ? '' : 's'}`;
            ti.tooltip = n.folder;
            ti.contextValue = n.removable ? 'pluginFolderRemovable' : 'pluginFolderLocked';
            ti.iconPath = new vscode.ThemeIcon(n.removable ? 'folder' : 'folder-library');
            ti.resourceUri = vscode.Uri.file(n.folder);
            return ti;
        }
        const p = n.info;
        const uses = this.instanceUseCounts.get(p.name) || 0;
        // Use count in the label so it survives description truncation.
        const label = uses > 0 ? `${p.name}  ×${uses}` : p.name;
        const ti = new vscode.TreeItem(label, vscode.TreeItemCollapsibleState.None);

        // Status icon: ok / warn / unloaded
        let icon = 'circle-outline';
        let color: vscode.ThemeColor | undefined;
        let statusHint = 'not loaded yet';
        if (p.loaded) {
            if (p.cert?.present && p.cert.valid) {
                icon = 'pass-filled';
                color = new vscode.ThemeColor('testing.iconPassed');
                statusHint = `certified ${p.cert.certified_at || ''}`.trim();
            } else if (p.cert?.present && !p.cert.valid) {
                icon = 'warning';
                color = new vscode.ThemeColor('testing.iconQueued');
                statusHint = 'cert stale (rebuild or re-cert needed)';
            } else {
                icon = 'circle-filled';
                statusHint = 'loaded, no cert file';
            }
        } else if (p.cert?.present && p.cert.valid) {
            icon = 'circle';
            statusHint = 'cert OK, not loaded yet';
        }

        ti.iconPath = new vscode.ThemeIcon(icon, color);
        ti.description = p.description;
        ti.tooltip = new vscode.MarkdownString(
            `**${p.name}**\n\n${p.description || ''}\n\n` +
            `Folder: \`${p.folder}\`\n\n` +
            `Status: ${statusHint}\n\n` +
            (p.has_ui ? 'Has UI panel\n\n' : 'No UI panel\n\n') +
            (uses ? `Used by ${uses} instance(s) in current project.` : 'Not in use.')
        );
        ti.contextValue = 'plugin';
        return ti;
    }

    getChildren(n?: Node): Node[] {
        if (!n) {
            if (this.plugins.length === 0) {
                return [{ kind: 'empty', label: 'No plugins discovered yet' }];
            }
            // Backend reports folder = <parent>/<pluginName>/ (the DLL's own
            // folder). The scan root is the PARENT of that — group by
            // parent so users see one collapsible node per scan source.
            const byParent = new Map<string, PluginInfo[]>();
            for (const p of this.plugins) {
                const parent = path.dirname(p.folder || '') || '<unknown>';
                if (!byParent.has(parent)) byParent.set(parent, []);
                byParent.get(parent)!.push(p);
            }
            return [...byParent.entries()].map(([folder, plugins]) => {
                const folderKey = path.resolve(folder).toLowerCase();
                return {
                    kind: 'folder',
                    folder,
                    plugins,
                    removable: this.removableFolders.has(folderKey),
                } as Node;
            });
        }
        if (n.kind === 'folder') {
            return n.plugins.map(info => ({ kind: 'plugin', info } as Node));
        }
        return [];
    }
}
