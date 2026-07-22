<svelte:options
  customElement={{
    tag: "xi-range",
    props: {
      low: { reflect: true, type: "Number" },
      high: { reflect: true, type: "Number" },
      min: { type: "Number" },
      max: { type: "Number" },
      step: { type: "Number" },
    },
  }}
/>

<script>
  // A [low, high] BAND — one control bound to two def keys (the schema's `key` and
  // `key2`). The common machine-vision intensity/size band, where two separate
  // sliders make the relationship invisible and let you cross them over.
  // Emits `change` with { low, high }; the handles can never pass each other.
  let { low = $bindable(0), high = $bindable(100), min = 0, max = 100, step = 1,
        label = "", disabled = false } = $props();
  const host = $host();
  const fire = () => host.dispatchEvent(
    new CustomEvent("change", { detail: { low, high, value: { low, high } },
                                bubbles: true, composed: true }));

  const clamp = (v) => Math.min(Math.max(Number(v), Number(min)), Number(max));
  // The invariant that makes a band a band: low <= high, enforced by pushing the
  // dragged handle instead of swapping (swapping makes the drag jump).
  function onLow(e) { low = Math.min(clamp(e.target.value), Number(high)); fire(); }
  function onHigh(e) { high = Math.max(clamp(e.target.value), Number(low)); fire(); }

  // Filled span between the handles, as % of the track.
  const pct = (v) => {
    const span = Number(max) - Number(min);
    return span > 0 ? ((Number(v) - Number(min)) / span) * 100 : 0;
  };
</script>

<div class="xi-range" class:disabled>
  {#if label}
    <div class="head"><span class="lbl">{label}</span><span class="val">{low} – {high}</span></div>
  {/if}
  <div class="track">
    <div class="fill" style="left:{pct(low)}%; right:{100 - pct(high)}%"></div>
    <input class="h lo" type="range" {min} {max} {step} value={low} {disabled}
           oninput={onLow} aria-label="{label} low" />
    <input class="h hi" type="range" {min} {max} {step} value={high} {disabled}
           oninput={onHigh} aria-label="{label} high" />
  </div>
</div>

<style>
  .xi-range { display: block; font: var(--xi-font, 13px system-ui, sans-serif);
    color: var(--xi-fg, inherit); }
  .head { display: flex; justify-content: space-between; margin-bottom: 0.3rem; }
  .val { color: var(--xi-accent, #3b82f6); font-variant-numeric: tabular-nums; }
  .track { position: relative; height: 1.6rem; }
  .track::before {
    content: ""; position: absolute; left: 0; right: 0; top: 50%;
    height: 4px; transform: translateY(-50%); border-radius: 2px;
    background: var(--xi-border, #ccc);
  }
  .fill {
    position: absolute; top: 50%; height: 4px; transform: translateY(-50%);
    border-radius: 2px; background: var(--xi-accent, #3b82f6);
  }
  /* Two natives stacked on one track; only the thumbs take pointer events, so
     each handle stays independently draggable. */
  .h {
    position: absolute; left: 0; right: 0; top: 0; width: 100%; margin: 0;
    height: 1.6rem; background: none; pointer-events: none;
    -webkit-appearance: none; appearance: none;
  }
  .h::-webkit-slider-thumb {
    -webkit-appearance: none; pointer-events: auto;
    width: 1.1rem; height: 1.1rem; border-radius: 50%;
    background: var(--xi-accent, #3b82f6); border: 2px solid var(--xi-bg, #fff);
    cursor: pointer;
  }
  .h::-moz-range-thumb {
    pointer-events: auto;
    width: 1.1rem; height: 1.1rem; border-radius: 50%; border: 2px solid var(--xi-bg, #fff);
    background: var(--xi-accent, #3b82f6); cursor: pointer;
  }
  .disabled { opacity: 0.5; }
</style>
