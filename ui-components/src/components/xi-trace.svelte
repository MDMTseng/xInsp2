<svelte:options
  customElement={{ tag: "xi-trace", props: { key: {}, label: {}, max: { type: "Number" } } }}
/>

<script>
  // Watch an output key across inspection runs (the "key trace"). Stays WS-
  // agnostic: the host feeds it via update(items) (a name->item map it builds
  // from whatever stream it owns); this component picks its `key`, shows the
  // latest value, and sparklines numeric history. Tap-out: method update(items);
  // readable `latest` / `history`; emits `sample` {value} on each new value.
  let { key = "", label = "", max = 60 } = $props();
  const host = $host();
  let canvas;
  let latest = $state(null);
  let history = $state([]);

  function draw() {
    if (!canvas) return;
    const ctx = canvas.getContext && canvas.getContext("2d");
    if (!ctx) return;                        // jsdom / no-canvas: data still tracked
    const W = (canvas.width = canvas.clientWidth || 120);
    const H = (canvas.height = canvas.clientHeight || 28);
    ctx.clearRect(0, 0, W, H);
    if (history.length < 2) return;
    const lo = Math.min(...history), hi = Math.max(...history), span = hi - lo || 1;
    ctx.beginPath();
    history.forEach((v, i) => {
      const x = (i / (history.length - 1)) * (W - 2) + 1;
      const y = H - 2 - ((v - lo) / span) * (H - 4);
      i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
    });
    ctx.strokeStyle = getComputedStyle(host).getPropertyValue("--xi-accent") || "#3b82f6";
    ctx.lineWidth = 1.5;
    ctx.stroke();
  }

  function update(items) {
    const it = items && items[key];
    if (!it) return;
    latest = it.value;
    if (typeof it.value === "number" && Number.isFinite(it.value)) {
      history = [...history, it.value].slice(-max);
      draw();
    }
    host.dispatchEvent(new CustomEvent("sample", { detail: { value: it.value }, bubbles: true, composed: true }));
  }

  $effect(() => {
    host.update = update;
    Object.defineProperty(host, "latest", { get: () => latest, configurable: true });
    Object.defineProperty(host, "history", { get: () => history.slice(), configurable: true });
    draw();
  });

  const fmt = (v) => (v == null ? "—" : typeof v === "number" ? (Number.isInteger(v) ? v : v.toFixed(3)) : String(v));
</script>

<div class="xi-trace">
  <span class="lbl">{label || key}</span>
  <canvas bind:this={canvas}></canvas>
  <span class="val">{fmt(latest)}</span>
</div>

<style>
  .xi-trace { display: inline-flex; align-items: center; gap: 0.5rem; font: 13px system-ui, sans-serif; color: var(--xi-fg, inherit); }
  canvas { width: 120px; height: 28px; display: block; }
  .val { min-width: 3em; text-align: right; font-variant-numeric: tabular-nums; }
</style>
