// numpad.mjs — the touch numeric-entry surface for the controls pluginlet.
//
// HOST-OWNED INPUT METHOD (doc 37, prior art: Qt Virtual Keyboard): there is ONE
// numpad for the whole page, not one baked into each widget. A numeric widget
// just declares "I want numeric entry" by calling openNumpad(); the surface, its
// layout, and its commit/cancel semantics live here. That keeps every numeric
// control's touch behaviour identical (the consistency lever) and keeps the
// widgets small.
//
// Vanilla DOM on purpose: no framework, no build step of its own, so it works in
// any host that loaded the plet's widgets.

let el = null;          // the singleton overlay
let state = null;       // { buf, onCommit, min, max }

const DIGITS = ["7", "8", "9", "4", "5", "6", "1", "2", "3", "±", "0", "."];

function ensure(doc) {
  if (el && el.ownerDocument === doc) return el;
  el = doc.createElement("div");
  el.className = "xi-numpad";
  el.setAttribute("role", "dialog");
  el.setAttribute("aria-label", "numeric keypad");
  el.style.cssText = [
    "position:fixed", "inset:0", "display:none", "align-items:center",
    "justify-content:center", "background:rgba(0,0,0,.45)", "z-index:2147483000",
  ].join(";");

  const card = doc.createElement("div");
  card.className = "xi-numpad-card";
  card.style.cssText = [
    "min-width:16rem", "padding:0.75rem", "border-radius:10px",
    "background:var(--xi-bg,#fff)", "color:var(--xi-fg,#111)",
    "border:1px solid var(--xi-border,#ccc)",
    "box-shadow:0 12px 40px rgba(0,0,0,.4)",
    "font:var(--xi-font,13px system-ui,sans-serif)",
  ].join(";");

  const cap = doc.createElement("div");
  cap.className = "xi-numpad-label";
  cap.style.cssText = "font-size:11px;opacity:.7;min-height:1.2em";

  const disp = doc.createElement("div");
  disp.className = "xi-numpad-display";
  disp.style.cssText = [
    "font-size:1.6rem", "text-align:right", "padding:0.35rem 0.5rem",
    "margin:0.25rem 0 0.5rem", "border:1px solid var(--xi-border,#ccc)",
    "border-radius:6px", "font-variant-numeric:tabular-nums", "min-height:2rem",
  ].join(";");

  const grid = doc.createElement("div");
  grid.style.cssText = "display:grid;grid-template-columns:repeat(3,1fr);gap:0.4rem";

  const mkKey = (text, cls, onClick) => {
    const b = doc.createElement("button");
    b.type = "button"; b.className = cls; b.textContent = text;
    // big hit targets — this exists for touch panels
    b.style.cssText = [
      "min-height:3rem", "font-size:1.15rem", "cursor:pointer",
      "color:var(--xi-fg,inherit)", "background:var(--xi-bg,#fff)",
      "border:1px solid var(--xi-border,#ccc)", "border-radius:8px",
    ].join(";");
    b.addEventListener("click", onClick);
    return b;
  };

  for (const d of DIGITS) grid.appendChild(mkKey(d, "xi-numpad-key", () => press(d, disp)));

  const row = doc.createElement("div");
  row.style.cssText = "display:grid;grid-template-columns:repeat(3,1fr);gap:0.4rem;margin-top:0.4rem";
  row.appendChild(mkKey("⌫", "xi-numpad-back", () => press("back", disp)));
  const cancel = mkKey("Cancel", "xi-numpad-cancel", () => close());
  const ok = mkKey("OK", "xi-numpad-ok", () => commit());
  ok.style.background = "var(--xi-accent,#3b82f6)"; ok.style.color = "#fff";
  row.appendChild(cancel); row.appendChild(ok);

  card.append(cap, disp, grid, row);
  el.appendChild(card);
  // tapping the backdrop cancels; taps inside the card must not
  el.addEventListener("click", (e) => { if (e.target === el) close(); });
  doc.body.appendChild(el);
  el._parts = { cap, disp };
  return el;
}

function press(k, disp) {
  if (!state) return;
  if (k === "back") state.buf = state.buf.slice(0, -1);
  else if (k === "±") state.buf = state.buf.startsWith("-") ? state.buf.slice(1) : "-" + state.buf;
  else if (k === "." && state.buf.includes(".")) { /* one decimal point only */ }
  else state.buf += k;
  disp.textContent = state.buf || "0";
}

function close() {
  if (el) el.style.display = "none";
  state = null;
}

// Commit CLAMPS to the declared range — the same bound the native set_def
// re-applies, so the panel can never send an out-of-range value (doc 37: the
// constraint is declared once and enforced on both sides).
function commit() {
  if (!state) return;
  const { buf, min, max, onCommit } = state;
  let v = Number(buf);
  if (buf === "" || buf === "-" || Number.isNaN(v)) return close();
  if (min != null && v < min) v = min;
  if (max != null && v > max) v = max;
  close();
  onCommit?.(v);
}

/**
 * Open the shared numpad for one numeric edit.
 * opts: { value, min, max, label, onCommit(value) } — onCommit fires only on OK.
 * A no-op outside a DOM (plain Node), so widgets can call it unconditionally.
 */
export function openNumpad(opts = {}) {
  const doc = opts.document || globalThis.document;
  if (!doc || !doc.body) return false;
  const node = ensure(doc);
  state = {
    buf: opts.value == null || opts.value === "" ? "" : String(opts.value),
    min: opts.min, max: opts.max, onCommit: opts.onCommit,
  };
  node._parts.cap.textContent = opts.label || "";
  node._parts.disp.textContent = state.buf || "0";
  node.style.display = "flex";
  return true;
}

/** Close the numpad without committing (host-side escape hatch). */
export function closeNumpad() { close(); }

/** Test seam: the current buffer, or null when closed. */
export function _numpadBuffer() { return state ? state.buf : null; }
