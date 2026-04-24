import * as vscode from 'vscode';

interface InstanceItem {
    name: string;
    plugin: string;
    def: Record<string, unknown>;
}

interface ParamItem {
    name: string;
    type: string;
    value: number | boolean;
    min?: number;
    max?: number;
}

type TreeItem = { kind: 'instance'; data: InstanceItem }
              | { kind: 'param';    data: ParamItem }
              | { kind: 'header';   label: string }
              | { kind: 'script';   label: string };

export class InstanceTreeProvider implements vscode.TreeDataProvider<TreeItem> {
    private _onDidChange = new vscode.EventEmitter<void>();
    readonly onDidChangeTreeData = this._onDidChange.event;

    private instances: InstanceItem[] = [];
    private params: ParamItem[] = [];
    private hasProject = false;

    update(instances: InstanceItem[], params: ParamItem[]): void {
        this.instances = instances;
        this.params = params;
        this._onDidChange.fire();
    }

    setProjectOpen(open: boolean) {
        this.hasProject = open;
        this._onDidChange.fire();
    }

    getTreeItem(element: TreeItem): vscode.TreeItem {
        if (element.kind === 'script') {
            const ti = new vscode.TreeItem(element.label, vscode.TreeItemCollapsibleState.None);
            ti.iconPath = new vscode.ThemeIcon('file-code');
            ti.contextValue = 'script';
            ti.tooltip = 'Open inspection.cpp';
            ti.command = { command: 'xinsp2.openScript', title: 'Open Inspection Script' };
            return ti;
        }
        if (element.kind === 'header') {
            const ti = new vscode.TreeItem(element.label, vscode.TreeItemCollapsibleState.Expanded);
            ti.contextValue = 'header';
            return ti;
        }
        if (element.kind === 'instance') {
            const ti = new vscode.TreeItem(element.data.name, vscode.TreeItemCollapsibleState.None);
            ti.description = element.data.plugin;
            ti.iconPath = new vscode.ThemeIcon('symbol-class');
            ti.contextValue = 'instance';
            ti.command = {
                command: 'xinsp2.openInstanceUI',
                title: 'Open UI',
                arguments: [element.data.name, element.data.plugin],
            };
            return ti;
        }
        // param
        const p = element.data;
        const ti = new vscode.TreeItem(p.name, vscode.TreeItemCollapsibleState.None);
        ti.description = `${p.value}`;
        ti.iconPath = new vscode.ThemeIcon('symbol-number');
        ti.contextValue = 'param';
        return ti;
    }

    getChildren(element?: TreeItem): TreeItem[] {
        if (!element) {
            const items: TreeItem[] = [];
            if (this.hasProject) items.push({ kind: 'script', label: 'inspection.cpp' });
            if (this.instances.length) items.push({ kind: 'header', label: 'Instances' });
            if (this.params.length)    items.push({ kind: 'header', label: 'Params' });
            return items;
        }
        if (element.kind === 'header' && element.label === 'Instances') {
            return this.instances.map(d => ({ kind: 'instance', data: d }));
        }
        if (element.kind === 'header' && element.label === 'Params') {
            return this.params.map(d => ({ kind: 'param', data: d }));
        }
        return [];
    }
}
