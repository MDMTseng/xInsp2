//
// monitor.mjs — export mode A (task #78): a read-only status monitor. Renders a
// grid of display tiles bound to OUTPUT keys (script VAR names) and feeds them
// from the live `vars` + binary preview streams. This is the generic, auto half
// of UI export — pick the `status`-tagged section items across a project and
// you have a monitoring wall. See docs/roadmap/webui-and-ui-export.md.
//
// item = { type: "value" | "trace" | "image", key, label? }
//   value → latest value text; trace → xi-trace sparkline; image → xi-image-viewer
//   fed by the preview frame whose gid matches the var's gid.
//

// Collapse a project's status sections (mountPanel-style descriptors, possibly
// per instance) into a flat item list for the monitor.
export function collectStatusItems(descriptors) {
  const items = [];
  for (const sec of descriptors || []) {
    if ((sec.tag || "") !== "status") continue;
    for (const c of sec.controls || []) {
      items.push({ type: c.type === "image" ? "image" : c.type === "trace" ? "trace" : "value",
                   key: c.key, label: c.label || c.key });
    }
  }
  return items;
}

export function mountMonitor(host, { client, items, columns = 3 }) {
  const doc = host.ownerDocument || globalThis.document;
  host.innerHTML = "";
  const grid = doc.createElement("div");
  grid.className = "xi-monitor";
  grid.style.display = "grid";
  grid.style.gap = "0.75rem";
  grid.style.gridTemplateColumns = `repeat(${columns}, minmax(0, 1fr))`;
  host.appendChild(grid);

  const byKey = new Map();        // key -> {type, el, gid?}
  // Image dedup: vars over one buffer share a canonical gid ("src") and the
  // backend sends a single preview frame per canon. Key viewers by canon and
  // map each var's gid → canon so the one frame fans out to every tile.
  const canonViewers = new Map(); // canon gid -> Set<viewer el>
  const gidToCanon = new Map();   // any var gid -> canon gid
  for (const it of items) {
    const tile = doc.createElement("div");
    tile.className = "xi-tile";
    tile.dataset.key = it.key;
    const lbl = doc.createElement("div");
    lbl.className = "xi-tile-label";
    lbl.textContent = it.label;
    tile.appendChild(lbl);

    let el;
    if (it.type === "trace") { el = doc.createElement("xi-trace"); el.setAttribute("key", it.key); }
    else if (it.type === "image") { el = doc.createElement("xi-image-viewer"); el.style.height = "180px"; }
    else { el = doc.createElement("div"); el.className = "xi-value"; el.textContent = "—"; }
    tile.appendChild(el);
    grid.appendChild(tile);
    byKey.set(it.key, { type: it.type, el });
  }

  const onVars = (v) => {
    const map = v.items || {};
    canonViewers.clear();
    gidToCanon.clear();
    for (const [key, slot] of byKey) {
      const it = map[key];
      if (!it) continue;
      if (slot.type === "trace") slot.el.update(map);
      else if (slot.type === "image") {
        if (it.gid != null) {
          const canon = it.src != null ? it.src : it.gid;
          gidToCanon.set(it.gid, canon);
          let set = canonViewers.get(canon);
          if (!set) canonViewers.set(canon, (set = new Set()));
          set.add(slot.el);
        }
      }
      else slot.el.textContent = fmtValue(it.value);
    }
  };
  const onPreview = (f) => {
    const canon = gidToCanon.has(f.gid) ? gidToCanon.get(f.gid) : f.gid;
    const viewers = canonViewers.get(canon);
    if (viewers) for (const viewer of viewers) viewer.setFrame(f.dataUrl);
  };
  const offVars = client.onVars(onVars);
  const offPreview = client.onPreview(onPreview);

  return { destroy() { offVars(); offPreview(); host.innerHTML = ""; } };
}

function fmtValue(v) {
  if (v == null) return "—";
  if (typeof v === "number") return Number.isInteger(v) ? String(v) : v.toFixed(3);
  if (typeof v === "boolean") return v ? "true" : "false";
  return String(v);
}
