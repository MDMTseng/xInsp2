// pluginlet-mount.mjs — the RUNTIME consumer of a plugin's `"pluginlets"`
// declaration (doc 37: "one declaration, three consumers").
//
//   plugin.json "pluginlets": ["controls"]
//        ├─ C++ build   → CMake xi_wire_pluginlets  (include + link the native half)
//        ├─ webui build → the xi-pluginlet-ui vite plugin (compile the UI half)
//        └─ RUNTIME     → THIS: the host reads the list off list_plugins and mounts
//                         each plet's UI, so a plugin gets a working panel with no
//                         bespoke UI of its own.
//
// This is APP-LAYER code: it depends on the plets (via the generated registry),
// never the reverse. The registry maps plet name → mount function and is built
// from each pluginlet.json's build.ui.entry + halves.ui.symbol, so adding a plet
// needs no edit here.

/**
 * Mount every pluginlet a plugin declares, in declaration order.
 *
 * opts:
 *   client      — needs getInstanceDef / setInstanceDef (+ exchangeInstance for buttons)
 *   instance    — the instance name to bind to
 *   pluginlets  — the plugin's declared list (from list_plugins' `pluginlets`)
 *   registry    — name → mount(host, {client, instance}); defaults to the generated one
 *   onMissing   — called with a plet name that has no registered UI (default: warn)
 *
 * Each plet gets its own child host element (class `xi-plet` + data-pluginlet), so
 * one panel can carry several plets without them fighting over `host`.
 * Returns { mounted: [{name, panel}], destroy() }.
 */
export async function mountPluginlets(host, opts) {
  const { client, instance, pluginlets = [], registry, onMissing } = opts;
  const doc = host.ownerDocument || globalThis.document;
  const reg = registry || (await defaultRegistry());
  const mounted = [];

  for (const name of pluginlets) {
    const mount = reg[name];
    if (typeof mount !== "function") {
      (onMissing || ((n) => console.warn(`[pluginlet] no UI registered for "${n}"`)))(name);
      continue;
    }
    const slot = doc.createElement("div");
    slot.className = "xi-plet";
    slot.dataset.pluginlet = name;
    host.appendChild(slot);
    // A plet's mount may legitimately decline (e.g. the controls plet returns null
    // when the instance declares no $schema) — keep the slot out of the way then.
    const panel = await mount(slot, { client, instance });
    if (panel) mounted.push({ name, panel });
    else slot.remove();
  }

  return {
    mounted,
    destroy() {
      for (const m of mounted) m.panel?.destroy?.();
      host.innerHTML = "";
    },
  };
}

// The generated registry is a build artifact (virtual:xi-pluginlet-registry). It
// is loaded lazily so this module stays importable in plain Node tests, where a
// registry is injected explicitly.
let _reg = null;
async function defaultRegistry() {
  if (_reg) return _reg;
  try {
    ({ PLUGINLET_UI: _reg } = await import("virtual:xi-pluginlet-registry"));
  } catch {
    _reg = {};   // no build-generated registry (plain Node / test): inject your own
  }
  return _reg;
}
