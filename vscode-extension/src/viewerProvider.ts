import * as vscode from 'vscode';

// Generic placeholder webview shell.
//
// This used to be the bespoke vars/preview viewer that decoded image previews
// and rendered exposed VARs. That model is gone: plugins now own their own
// webUI and are hosted generically (has_ui → `<folder>/ui`). The extension keeps
// no preview/vars/gid decoding. This shell remains only so the type still
// compiles for any incidental import; it is no longer wired into the extension.
export class ViewerProvider implements vscode.WebviewViewProvider {
    public static readonly viewType = 'xinsp2.viewer';
    private view?: vscode.WebviewView;

    constructor(private extensionUri: vscode.Uri) {}

    /** True once the viewer panel has been opened at least once. */
    isResolved(): boolean { return !!this.view; }

    resolveWebviewView(webviewView: vscode.WebviewView): void {
        this.view = webviewView;
        webviewView.webview.options = { enableScripts: false };
        webviewView.webview.html = this.getHtml();
    }

    private getHtml(): string {
        return `<!DOCTYPE html>
<html>
<head><meta charset="utf-8"></head>
<body style="font-family: var(--vscode-font-family); padding: 8px; opacity: 0.6;">
    Open a plugin instance UI to view its data.
</body>
</html>`;
    }
}
