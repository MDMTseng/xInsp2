// Fan out the freshly built kit bundle to the in-repo consumers that vendor a
// copy rather than resolve @xinsp2/components from node_modules:
//   - hmi/lib             : the HMI's standalone script tag
//   - vscode-extension/media : the extension's plugin-UI webview host, which
//     also needs the VS Code theme adapter alongside the bundle.
// Run automatically by `npm run build`.
import { copyFileSync, mkdirSync } from "node:fs";
import { dirname } from "node:path";

const copies = [
  ["dist/xi-components.esm.js", "../hmi/lib/xi-components.esm.js"],
  ["dist/xi-components.esm.js", "../vscode-extension/media/xi-components.esm.js"],
  ["src/vscode-theme.css", "../vscode-extension/media/vscode-theme.css"],
];

for (const [from, to] of copies) {
  mkdirSync(dirname(to), { recursive: true });
  copyFileSync(from, to);
  console.log(`[stage-bundles] ${from} -> ${to}`);
}
