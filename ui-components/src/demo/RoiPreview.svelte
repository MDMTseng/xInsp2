<script>
  // Child of TeachPanel — here to prove plain-Svelte component COMPOSITION inside
  // a registered widget: a normal import, normal props, no custom-element
  // boundary between parent and child.
  let { roi, img = { w: 320, h: 180 } } = $props();
  const pct = (v, base) => Math.max(0, Math.min(100, (100 * v) / base));
</script>

<div class="stage">
  <div class="box"
       style="left:{pct(roi.x, img.w)}%; top:{pct(roi.y, img.h)}%;
              width:{pct(roi.w, img.w)}%; height:{pct(roi.h, img.h)}%">
    <span class="tag">{roi.w}×{roi.h}</span>
  </div>
</div>

<style>
  .stage {
    position: relative; width: 100%; aspect-ratio: 16 / 9;
    background:
      linear-gradient(45deg, var(--xi-border, #2b3444) 1px, transparent 1px) 0 0 / 24px 24px,
      var(--xi-bg, #05070c);
    border: 1px solid var(--xi-border, #2b3444); border-radius: 6px; overflow: hidden;
  }
  .box {
    position: absolute; border: 2px solid var(--xi-accent, #3b82f6);
    background: color-mix(in srgb, var(--xi-accent, #3b82f6) 15%, transparent);
    border-radius: 2px;
  }
  .tag {
    position: absolute; right: 2px; bottom: 2px; font-size: 10px;
    color: var(--xi-accent, #3b82f6); font-variant-numeric: tabular-nums;
  }
</style>
