// _dom.mjs — minimal jsdom global setup so the BUILT custom elements (which call
// customElements.define + use Element/HTMLElement/document at import) run in node.
import { JSDOM } from "jsdom";

export function setupDom() {
  const dom = new JSDOM("<!doctype html><html><body></body></html>", { pretendToBeVisual: true });
  const w = dom.window;
  globalThis.window = w;
  for (const k of Object.getOwnPropertyNames(w)) {
    if (k in globalThis) continue;
    const d = Object.getOwnPropertyDescriptor(w, k);
    try { Object.defineProperty(globalThis, k, d); } catch { /* getter-only */ }
  }
  // node 22 ships its own Event/CustomEvent/etc.; force jsdom's so events
  // constructed here share the same realm as the elements (else dispatchEvent
  // rejects a cross-realm Event).
  for (const k of ["Event", "CustomEvent", "Node", "Element", "HTMLElement",
                   "DocumentFragment", "ShadowRoot", "document", "customElements"]) {
    const d = Object.getOwnPropertyDescriptor(w, k);
    if (d) { try { Object.defineProperty(globalThis, k, d); } catch { /* getter-only */ } }
  }
  globalThis.requestAnimationFrame = (cb) => w.setTimeout(() => cb(Date.now()), 0);
  // jsdom lacks ResizeObserver; the viewer/editor use it. No-op stub.
  if (!globalThis.ResizeObserver) {
    globalThis.ResizeObserver = class { observe() {} unobserve() {} disconnect() {} };
    w.ResizeObserver = globalThis.ResizeObserver;
  }
  return w;
}

export const tick = (w) => new Promise((r) => w.setTimeout(r, 0));
