<script>
  // TeachPanel — the REFERENCE custom widget for the controls plet registry
  // (doc 37): a plain (non-custom-element) Svelte component tree that a
  // registerWidget() factory mounts into one grid cell. Everything in here is
  // ordinary Svelte 5 — child components (RoiPreview), runes, events — AND the
  // plet's xi-* custom elements mixed in as plain tags, so standard controls and
  // a bespoke surface share one panel without a framework boundary.
  //
  // Def contract: reads/writes ONE declared key (node.key, e.g. "roi") holding
  // "x,y,w,h". Edits stay local until Apply pushes through pushDef — the same
  // validated set_instance_def path every built-in widget uses.
  import RoiPreview from "./RoiPreview.svelte";

  let { values, pushDef, node } = $props();
  const key = node?.key || "roi";

  const parse = (s) => {
    const [x, y, w, h] = String(s ?? "").split(",").map(Number);
    return { x: x || 0, y: y || 0, w: w || 120, h: h || 80 };
  };
  // Local editing copy, re-seeded whenever the def value changes (panel refresh).
  let roi = $state(parse(values[key]));
  $effect(() => { roi = parse(values[key]); });

  const dirty = () => `${roi.x},${roi.y},${roi.w},${roi.h}` !== String(values[key] ?? "");
  const edit = (f) => (e) => { roi = { ...roi, [f]: Number(e.detail.value) || 0 }; };
</script>

<div class="teach">
  <div class="head">
    <span class="title">Teach ROI</span>
    <span class="key">def: {key}</span>
  </div>
  <RoiPreview {roi} />
  <div class="fields">
    <!-- the plet's custom elements, used as plain tags inside Svelte -->
    <xi-stepper label="X" min="0" max="320" step="4" value={roi.x} onchange={edit("x")}></xi-stepper>
    <xi-stepper label="Y" min="0" max="180" step="4" value={roi.y} onchange={edit("y")}></xi-stepper>
    <xi-stepper label="W" min="8" max="320" step="4" value={roi.w} onchange={edit("w")}></xi-stepper>
    <xi-stepper label="H" min="8" max="180" step="4" value={roi.h} onchange={edit("h")}></xi-stepper>
  </div>
  <button class="apply" disabled={!dirty()}
          onclick={() => pushDef(key, `${roi.x},${roi.y},${roi.w},${roi.h}`)}>
    Apply ROI
  </button>
</div>

<style>
  .teach { display: grid; gap: 10px; font: var(--xi-font, 13px system-ui, sans-serif);
    color: var(--xi-fg, inherit); }
  .head { display: flex; justify-content: space-between; align-items: baseline; }
  .title { font-weight: 600; }
  .key { font-size: 10px; opacity: 0.6; font-family: monospace; }
  .fields { display: grid; grid-template-columns: repeat(2, 1fr); gap: 8px; }
  .apply {
    padding: 8px; border-radius: 7px; cursor: pointer; font-weight: 600;
    color: #fff; background: var(--xi-accent, #3b82f6); border: none;
  }
  .apply:disabled { opacity: 0.35; cursor: default; }
</style>
