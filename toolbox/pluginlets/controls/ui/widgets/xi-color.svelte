<svelte:options
  customElement={{
    tag: "xi-color",
    props: { value: { reflect: true, type: "String" } },
  }}
/>

<script>
  // Hex colour. The native picker does the hard part; we add the swatch + the hex
  // readout so the value is legible on a panel (and matches what set_def stores).
  let { value = $bindable("#000000"), label = "", disabled = false } = $props();
  const host = $host();
  const fire = (t) => host.dispatchEvent(
    new CustomEvent(t, { detail: { value }, bubbles: true, composed: true }));
  function onInput(e) { value = e.target.value; fire("input"); }
  function onChange(e) { value = e.target.value; fire("change"); }
</script>

<label class="xi-color">
  {#if label}<span class="lbl">{label}</span>{/if}
  <span class="wrap">
    <input type="color" {value} {disabled} oninput={onInput} onchange={onChange} />
    <code class="hex">{value}</code>
  </span>
</label>

<style>
  .xi-color { display: inline-flex; align-items: center; gap: 0.5rem;
    font: var(--xi-font, 13px system-ui, sans-serif); color: var(--xi-fg, inherit); }
  .wrap { display: inline-flex; align-items: center; gap: 0.4rem; }
  input[type="color"] {
    /* a big square swatch — touch-friendly, and reads as a colour, not a field */
    width: 2.2rem; height: 2.2rem; padding: 0; cursor: pointer;
    background: none; border: 1px solid var(--xi-border, #ccc);
    border-radius: var(--xi-radius, 3px);
  }
  .hex { font-variant-numeric: tabular-nums; opacity: 0.8; }
</style>
