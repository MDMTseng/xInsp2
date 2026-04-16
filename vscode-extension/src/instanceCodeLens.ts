import * as vscode from 'vscode';

// Matches patterns like:
//   xi::Instance<MockCamera> cam{"cam0"};
//   xi::Instance<xi::TestImageSource> cam{"cam0"};
//   static xi::Instance<Scaler> scaler{"my_scaler"};
//   static auto& cam_source() { ... }  — won't match (good)
const INSTANCE_RE = /xi::Instance<([^>]+)>\s+(\w+)\s*\{\s*"([^"]+)"/g;

// Also match Param declarations:
//   xi::Param<int> blur_radius{"blur_radius", 2, {0, 10}};
const PARAM_RE = /xi::Param<([^>]+)>\s+(\w+)\s*\{\s*"([^"]+)"/g;

export class InstanceCodeLensProvider implements vscode.CodeLensProvider {
    private _onDidChange = new vscode.EventEmitter<void>();
    readonly onDidChangeCodeLenses = this._onDidChange.event;

    refresh(): void {
        this._onDidChange.fire();
    }

    provideCodeLenses(document: vscode.TextDocument): vscode.CodeLens[] {
        if (!document.fileName.endsWith('.cpp')) return [];

        const text = document.getText();
        const lenses: vscode.CodeLens[] = [];

        // Instance declarations → "⚙ Configure <name>"
        let m: RegExpExecArray | null;
        INSTANCE_RE.lastIndex = 0;
        while ((m = INSTANCE_RE.exec(text)) !== null) {
            const line = document.positionAt(m.index).line;
            const range = new vscode.Range(line, 0, line, 0);
            const pluginType = m[1].trim();
            const instanceName = m[3];

            // Derive plugin name from type (strip xi:: prefix, lowercase)
            let pluginName = pluginType
                .replace(/^xi::/, '')
                .replace(/^std::/, '');
            // Common mappings
            if (pluginName === 'TestImageSource') pluginName = 'mock_camera';
            if (pluginName === 'ImageSource') pluginName = 'mock_camera';

            lenses.push(new vscode.CodeLens(range, {
                title: `⚙ Configure ${instanceName}`,
                command: 'xinsp2.openInstanceUI',
                arguments: [instanceName, pluginName],
            }));
        }

        // Param declarations → "🎚 Tune <name>"
        PARAM_RE.lastIndex = 0;
        while ((m = PARAM_RE.exec(text)) !== null) {
            const line = document.positionAt(m.index).line;
            const range = new vscode.Range(line, 0, line, 0);
            const paramName = m[3];
            lenses.push(new vscode.CodeLens(range, {
                title: `🎚 ${paramName}`,
                command: 'xinsp2.focusParam',
                arguments: [paramName],
                tooltip: `Click to focus param ${paramName} in the sidebar`,
            }));
        }

        return lenses;
    }
}
