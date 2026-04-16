import * as vscode from 'vscode';
import * as path from 'path';

export class ViewerProvider implements vscode.WebviewViewProvider {
    public static readonly viewType = 'xinsp2.viewer';
    private view?: vscode.WebviewView;

    constructor(private extensionUri: vscode.Uri) {}

    resolveWebviewView(webviewView: vscode.WebviewView): void {
        this.view = webviewView;
        webviewView.webview.options = {
            enableScripts: true,
            localResourceRoots: [vscode.Uri.joinPath(this.extensionUri, 'webview')],
        };
        webviewView.webview.html = this.getHtml();
    }

    postVars(vars: unknown): void {
        this.view?.webview.postMessage({ type: 'vars', data: vars });
    }

    highlightVar(varName: string): void {
        this.view?.webview.postMessage({ type: 'highlight', name: varName });
    }

    postPreview(gid: number, width: number, height: number, jpegBase64: string): void {
        this.view?.webview.postMessage({
            type: 'preview',
            gid, width, height,
            jpeg: jpegBase64,
        });
    }

    private getHtml(): string {
        return `<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
    body { font-family: var(--vscode-font-family); font-size: 13px; color: var(--vscode-foreground); padding: 8px; margin: 0; }
    h3 { margin: 4px 0; font-size: 13px; font-weight: 600; }
    table { width: 100%; border-collapse: collapse; margin-bottom: 12px; }
    th, td { text-align: left; padding: 3px 6px; border-bottom: 1px solid var(--vscode-widget-border); }
    th { font-weight: 600; opacity: 0.7; }
    .kind { opacity: 0.5; font-size: 11px; }
    .img-preview { max-width: 100%; border: 1px solid var(--vscode-widget-border); margin: 4px 0; cursor: pointer; }
    .img-label { font-size: 11px; opacity: 0.6; }
    #no-data { opacity: 0.5; font-style: italic; }
    tr.highlighted { background: var(--vscode-editor-selectionBackground, rgba(38,79,120,0.5)); }
</style>
</head>
<body>
    <div id="no-data">No inspection data yet. Run an inspection to see results.</div>
    <div id="vars-section" style="display:none">
        <h3>Variables</h3>
        <table><thead><tr><th>Name</th><th>Kind</th><th>Value</th></tr></thead><tbody id="vars-body"></tbody></table>
    </div>
    <div id="images-section" style="display:none">
        <h3>Image Preview</h3>
        <div id="images-container"></div>
    </div>
<script>
const vscode = acquireVsCodeApi();
const imageMap = {};

window.addEventListener('message', (e) => {
    const msg = e.data;
    if (msg.type === 'vars') {
        renderVars(msg.data);
    } else if (msg.type === 'preview') {
        renderPreview(msg.gid, msg.width, msg.height, msg.jpeg);
    } else if (msg.type === 'highlight') {
        highlightVar(msg.name);
    }
});

function highlightVar(name) {
    // Remove old highlights
    document.querySelectorAll('tr.highlighted').forEach(el => el.classList.remove('highlighted'));
    // Find and highlight the row + scroll to it
    const rows = document.querySelectorAll('#vars-body tr');
    for (const row of rows) {
        if (row.children[0]?.textContent === name) {
            row.classList.add('highlighted');
            row.scrollIntoView({ behavior: 'smooth', block: 'center' });
            // Also show the image if it's an image var
            const gidMatch = row.dataset?.gid;
            if (gidMatch) {
                const imgDiv = document.getElementById('img-' + gidMatch);
                if (imgDiv) imgDiv.scrollIntoView({ behavior: 'smooth', block: 'center' });
            }
            break;
        }
    }
}

function renderVars(vars) {
    document.getElementById('no-data').style.display = 'none';
    document.getElementById('vars-section').style.display = 'block';
    const tbody = document.getElementById('vars-body');
    tbody.innerHTML = '';
    const imgContainer = document.getElementById('images-container');
    imgContainer.innerHTML = '';

    let hasImages = false;
    for (const item of vars.items) {
        const tr = document.createElement('tr');
        const tdName = document.createElement('td');
        tdName.textContent = item.name;
        const tdKind = document.createElement('td');
        tdKind.innerHTML = '<span class="kind">' + item.kind + '</span>';
        const tdVal = document.createElement('td');

        if (item.kind === 'image') {
            tdVal.textContent = item.gid + ' (' + (item.raw ? 'raw' : 'jpeg') + ')';
            hasImages = true;
            // Placeholder for the image
            const div = document.createElement('div');
            div.id = 'img-' + item.gid;
            const label = document.createElement('div');
            label.className = 'img-label';
            label.textContent = item.name;
            div.appendChild(label);
            imgContainer.appendChild(div);
        } else if (item.kind === 'string') {
            tdVal.textContent = '"' + item.value + '"';
        } else if (item.kind === 'boolean') {
            tdVal.textContent = item.value ? 'true' : 'false';
        } else {
            tdVal.textContent = JSON.stringify(item.value);
        }

        tr.appendChild(tdName);
        tr.appendChild(tdKind);
        tr.appendChild(tdVal);
        tbody.appendChild(tr);
    }
    document.getElementById('images-section').style.display = hasImages ? 'block' : 'none';
}

function renderPreview(gid, width, height, jpegBase64) {
    const container = document.getElementById('img-' + gid);
    if (!container) return;
    let img = container.querySelector('img');
    if (!img) {
        img = document.createElement('img');
        img.className = 'img-preview';
        container.appendChild(img);
    }
    img.src = 'data:image/jpeg;base64,' + jpegBase64;
    img.title = width + 'x' + height;

    const label = container.querySelector('.img-label');
    if (label) label.textContent += ' (' + width + 'x' + height + ')';
}
</script>
</body>
</html>`;
    }
}
