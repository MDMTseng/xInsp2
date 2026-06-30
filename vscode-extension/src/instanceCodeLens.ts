import * as vscode from 'vscode';

// --- Declaration patterns (file scope) ---
const INSTANCE_RE = /xi::Instance<([^>]+)>\s+(\w+)\s*\{\s*"([^"]+)"/g;
const PARAM_RE    = /xi::Param<([^>]+)>\s+(\w+)\s*\{\s*"([^"]+)"/g;


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

        // First pass: collect declared instance and param names
        const instances = new Map<string, { varName: string; pluginType: string; instanceName: string }>();
        const params = new Map<string, string>();  // varName → paramName

        let m: RegExpExecArray | null;

        INSTANCE_RE.lastIndex = 0;
        while ((m = INSTANCE_RE.exec(text)) !== null) {
            const pluginType = m[1].trim();
            const varName = m[2];
            const instanceName = m[3];
            instances.set(varName, { varName, pluginType, instanceName });

            let pluginName = pluginType.replace(/^xi::/, '');
            if (pluginName === 'TestImageSource' || pluginName === 'ImageSource') pluginName = 'mock_camera';

            const line = document.positionAt(m.index).line;
            lenses.push(new vscode.CodeLens(new vscode.Range(line, 0, line, 0), {
                title: `⚙ Configure ${instanceName}`,
                command: 'xinsp2.openInstanceUI',
                arguments: [instanceName, pluginName],
            }));
        }

        PARAM_RE.lastIndex = 0;
        while ((m = PARAM_RE.exec(text)) !== null) {
            const varName = m[2];
            const paramName = m[3];
            params.set(varName, paramName);

            const line = document.positionAt(m.index).line;
            lenses.push(new vscode.CodeLens(new vscode.Range(line, 0, line, 0), {
                title: `🎚 Tune ${paramName}`,
                command: 'xinsp2.focusParam',
                arguments: [paramName],
            }));
        }

        // Second pass: scan for usages in function bodies
        // Instance variable usages: cam->grab(), cam.something()
        for (const [varName, info] of instances) {
            const usageRe = new RegExp(`\\b${varName}\\s*->`, 'g');
            while ((m = usageRe.exec(text)) !== null) {
                const line = document.positionAt(m.index).line;
                const declLine = this.findDeclLine(text, document, varName, INSTANCE_RE);
                if (line === declLine) continue; // skip declaration itself

                let pluginName = info.pluginType.replace(/^xi::/, '');
                if (pluginName === 'TestImageSource' || pluginName === 'ImageSource') pluginName = 'mock_camera';

                lenses.push(new vscode.CodeLens(new vscode.Range(line, 0, line, 0), {
                    title: `⚙ ${info.instanceName}`,
                    command: 'xinsp2.openInstanceUI',
                    arguments: [info.instanceName, pluginName],
                }));
            }
        }

        // Param variable usages inside function calls
        for (const [varName, paramName] of params) {
            // Look for the param var used as a function argument or in an expression
            // Match: someFunc(gray, blur_radius) or  frame * static_cast<int>(blur_radius)
            const usageRe = new RegExp(`[,(]\\s*${varName}\\b|\\b${varName}\\s*[),+\\-*/]`, 'g');
            const seen = new Set<number>();
            while ((m = usageRe.exec(text)) !== null) {
                const line = document.positionAt(m.index).line;
                const declLine = this.findDeclLine(text, document, varName, PARAM_RE);
                if (line === declLine || seen.has(line)) continue;
                seen.add(line);

                lenses.push(new vscode.CodeLens(new vscode.Range(line, 0, line, 0), {
                    title: `🎚 ${paramName}`,
                    command: 'xinsp2.focusParam',
                    arguments: [paramName],
                }));
            }
        }

        return lenses;
    }

    private findDeclLine(text: string, doc: vscode.TextDocument, varName: string, re: RegExp): number {
        re.lastIndex = 0;
        let m: RegExpExecArray | null;
        while ((m = re.exec(text)) !== null) {
            if (m[2] === varName) return doc.positionAt(m.index).line;
        }
        return -1;
    }
}
