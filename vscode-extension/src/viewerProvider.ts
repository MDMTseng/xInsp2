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
    #no-data { opacity: 0.5; font-style: italic; }

    .var-item { border-bottom: 1px solid var(--vscode-widget-border, #333); padding: 6px 0; }
    .var-header { display: flex; align-items: center; gap: 6px; cursor: pointer; padding: 2px 4px; border-radius: 3px; }
    .var-header:hover { background: var(--vscode-list-hoverBackground, rgba(255,255,255,0.05)); }
    .var-header.highlighted { background: var(--vscode-editor-selectionBackground, rgba(38,79,120,0.5)); }
    .var-name { font-weight: 600; }
    .var-kind { opacity: 0.4; font-size: 11px; }
    .var-value { opacity: 0.8; margin-left: auto; font-family: var(--vscode-editor-font-family, monospace); font-size: 12px; }
    .var-badge-pass { background: #2d5a2d; color: #6fbf6f; padding: 1px 6px; border-radius: 8px; font-size: 10px; font-weight: 600; }
    .var-badge-fail { background: #5a2d2d; color: #bf6f6f; padding: 1px 6px; border-radius: 8px; font-size: 10px; font-weight: 600; }

    .var-body { margin-left: 16px; padding: 4px 0; display: none; }
    .var-body.open { display: block; }
    .var-toggle { font-size: 10px; opacity: 0.5; width: 14px; text-align: center; }

    .data-tree { margin: 4px 0 4px 8px; }
    .data-row { padding: 1px 0; font-size: 12px; }
    .data-key { opacity: 0.6; }
    .data-val { font-family: var(--vscode-editor-font-family, monospace); }
    .data-val.str { color: #ce9178; }
    .data-val.num { color: #b5cea8; }
    .data-val.bool { color: #569cd6; }

    .img-row { display: flex; gap: 8px; flex-wrap: wrap; margin: 4px 0; }
    .img-thumb { border: 1px solid var(--vscode-widget-border, #444); cursor: pointer; max-width: 120px; max-height: 90px; background: #000; }
    .img-thumb-label { font-size: 10px; opacity: 0.5; text-align: center; }
    .img-full { max-width: 100%; border: 1px solid var(--vscode-widget-border); margin: 4px 0; background: #000; }

    .array-header { cursor: pointer; opacity: 0.6; font-size: 11px; padding: 2px 0; }
    .array-header:hover { opacity: 1; }
    .array-item { margin-left: 12px; border-left: 2px solid var(--vscode-widget-border, #333); padding-left: 8px; margin-bottom: 4px; }

    #preview-panel { margin-top: 12px; border-top: 1px solid var(--vscode-widget-border); padding-top: 8px; }
    #preview-label { font-size: 11px; opacity: 0.6; margin-bottom: 4px; }
</style>
</head>
<body>
    <div id="no-data">No inspection data yet. Run an inspection to see results.</div>
    <div id="vars-list"></div>
    <div id="preview-panel" style="display:none">
        <div id="preview-label"></div>
        <img id="preview-img" class="img-full" style="display:none">
    </div>
<script>
const vscode = acquireVsCodeApi();

window.addEventListener('message', (e) => {
    const msg = e.data;
    if (msg.type === 'vars') renderVars(msg.data);
    else if (msg.type === 'preview') renderPreview(msg.gid, msg.width, msg.height, msg.jpeg);
    else if (msg.type === 'highlight') highlightVar(msg.name);
});

function highlightVar(name) {
    document.querySelectorAll('.var-header.highlighted').forEach(el => el.classList.remove('highlighted'));
    const el = document.querySelector('[data-var-name="' + name + '"]');
    if (el) {
        el.classList.add('highlighted');
        el.scrollIntoView({ behavior: 'smooth', block: 'center' });
        // Open the body if collapsed
        const body = el.nextElementSibling;
        if (body && !body.classList.contains('open')) body.classList.add('open');
    }
}

function renderVars(vars) {
    document.getElementById('no-data').style.display = 'none';
    const list = document.getElementById('vars-list');
    list.innerHTML = '';

    for (const item of vars.items) {
        const div = document.createElement('div');
        div.className = 'var-item';

        if (item.kind === 'record') {
            div.innerHTML = renderRecord(item.name, item);
        } else if (item.kind === 'image') {
            div.innerHTML = renderImageVar(item);
        } else {
            div.innerHTML = renderScalar(item);
        }
        list.appendChild(div);
    }

    // Wire up toggles
    list.querySelectorAll('.var-header').forEach(hdr => {
        hdr.addEventListener('click', () => {
            const body = hdr.nextElementSibling;
            if (body) {
                body.classList.toggle('open');
                const toggle = hdr.querySelector('.var-toggle');
                if (toggle) toggle.textContent = body.classList.contains('open') ? '▾' : '▸';
            }
        });
    });

    // Wire up image thumbnails
    list.querySelectorAll('.img-thumb').forEach(img => {
        img.addEventListener('click', () => {
            document.getElementById('preview-panel').style.display = 'block';
            document.getElementById('preview-img').src = img.src;
            document.getElementById('preview-img').style.display = 'block';
            document.getElementById('preview-label').textContent = img.title;
        });
    });
}

function renderScalar(item) {
    let valHtml = '';
    let badge = '';
    if (item.kind === 'boolean') {
        valHtml = '<span class="data-val bool">' + item.value + '</span>';
        if (item.name === 'pass' || item.name === 'all_pass' || item.name.endsWith('_pass')) {
            badge = item.value
                ? '<span class="var-badge-pass">PASS</span>'
                : '<span class="var-badge-fail">FAIL</span>';
        }
    } else if (item.kind === 'string') {
        valHtml = '<span class="data-val str">"' + item.value + '"</span>';
    } else if (item.kind === 'number') {
        valHtml = '<span class="data-val num">' + item.value + '</span>';
    } else {
        valHtml = '<span class="data-val">' + JSON.stringify(item.value) + '</span>';
    }
    return '<div class="var-header" data-var-name="' + item.name + '">' +
        '<span class="var-name">' + item.name + '</span>' +
        '<span class="var-kind">' + item.kind + '</span>' +
        badge +
        '<span class="var-value">' + valHtml + '</span></div>';
}

function renderImageVar(item) {
    return '<div class="var-header" data-var-name="' + item.name + '">' +
        '<span class="var-name">' + item.name + '</span>' +
        '<span class="var-kind">image</span>' +
        '<span class="var-value" style="opacity:0.4">#' + item.gid + '</span></div>' +
        '<div class="var-body open"><div id="img-' + item.gid + '">' +
        '<div class="img-thumb-label">' + item.name + '</div></div></div>';
}

function renderRecord(name, item) {
    const data = item.data || {};
    const images = item.images || {};
    const imageKeys = item.image_keys || [];
    let hasPass = data.pass !== undefined;
    let badge = '';
    if (hasPass) {
        badge = data.pass
            ? '<span class="var-badge-pass">PASS</span>'
            : '<span class="var-badge-fail">FAIL</span>';
    }

    let html = '<div class="var-header" data-var-name="' + name + '">' +
        '<span class="var-toggle">▸</span>' +
        '<span class="var-name">' + name + '</span>' +
        '<span class="var-kind">record</span>' + badge +
        '<span class="var-value" style="opacity:0.3">{' + Object.keys(data).length + ' fields, ' + imageKeys.length + ' images}</span></div>';

    html += '<div class="var-body">';

    // Images as thumbnails
    if (imageKeys.length > 0) {
        html += '<div class="img-row">';
        for (const ik of imageKeys) {
            const gid = images[ik];
            html += '<div><div id="img-' + gid + '"><img class="img-thumb" data-gid="' + gid + '" title="' + name + '.' + ik + '"></div>' +
                '<div class="img-thumb-label">' + ik + '</div></div>';
        }
        html += '</div>';
    }

    // Data fields
    html += '<div class="data-tree">';
    html += renderJsonTree(data);
    html += '</div>';

    html += '</div>';
    return html;
}

function renderJsonTree(obj, depth) {
    depth = depth || 0;
    let html = '';
    if (Array.isArray(obj)) {
        for (let i = 0; i < obj.length; ++i) {
            if (typeof obj[i] === 'object' && obj[i] !== null) {
                html += '<div class="array-item">';
                html += '<div class="array-header">[' + i + ']</div>';
                html += renderJsonTree(obj[i], depth + 1);
                html += '</div>';
            } else {
                html += '<div class="data-row"><span class="data-key">[' + i + ']</span> ' + renderValue(obj[i]) + '</div>';
            }
        }
    } else if (typeof obj === 'object' && obj !== null) {
        for (const [k, v] of Object.entries(obj)) {
            if (typeof v === 'object' && v !== null) {
                if (Array.isArray(v)) {
                    html += '<div class="data-row"><span class="data-key">' + k + '</span> <span style="opacity:0.4">[' + v.length + ' items]</span></div>';
                    html += '<div style="margin-left:12px">' + renderJsonTree(v, depth + 1) + '</div>';
                } else {
                    html += '<div class="data-row"><span class="data-key">' + k + ':</span></div>';
                    html += '<div style="margin-left:12px">' + renderJsonTree(v, depth + 1) + '</div>';
                }
            } else {
                let badge = '';
                if ((k === 'pass' || k.endsWith('_pass')) && typeof v === 'boolean') {
                    badge = v ? ' <span class="var-badge-pass">PASS</span>' : ' <span class="var-badge-fail">FAIL</span>';
                }
                html += '<div class="data-row"><span class="data-key">' + k + ':</span> ' + renderValue(v) + badge + '</div>';
            }
        }
    }
    return html;
}

function renderValue(v) {
    if (typeof v === 'string') return '<span class="data-val str">"' + v + '"</span>';
    if (typeof v === 'number') return '<span class="data-val num">' + v + '</span>';
    if (typeof v === 'boolean') return '<span class="data-val bool">' + v + '</span>';
    if (v === null) return '<span class="data-val" style="opacity:0.3">null</span>';
    return '<span class="data-val">' + JSON.stringify(v) + '</span>';
}

function renderPreview(gid, width, height, jpegBase64) {
    const el = document.querySelector('[data-gid="' + gid + '"]');
    if (el) {
        el.src = 'data:image/jpeg;base64,' + jpegBase64;
        el.title = (el.title || '') + ' (' + width + 'x' + height + ')';
        return;
    }
    // Fallback: find by container id
    const container = document.getElementById('img-' + gid);
    if (!container) return;
    let img = container.querySelector('img');
    if (!img) {
        img = document.createElement('img');
        img.className = 'img-thumb';
        img.dataset.gid = gid;
        container.appendChild(img);
    }
    img.src = 'data:image/jpeg;base64,' + jpegBase64;
    img.title = width + 'x' + height;
}
</script>
</body>
</html>`;
    }
}
