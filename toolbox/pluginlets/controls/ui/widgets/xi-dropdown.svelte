<svelte:options
  customElement={{
    tag: "xi-dropdown",
    props: { value: { reflect: true }, options: {} },
  }}
/>

<script>
  // Key-bound select. `options` is an array (JS property) or JSON string
  // attribute of values / {value,label}. Exposes `value`; emits `change`.
  import { parseOptions } from "../lib/options.mjs";
  let { value = $bindable(""), options = [], label = "", disabled = false } = $props();
  const host = $host();
  const opts = $derived(parseOptions(options));
  function onChange(e) {
    value = e.target.value;
    host.dispatchEvent(new CustomEvent("change", { detail: { value }, bubbles: true, composed: true }));
  }
</script>

<label class="xi-dropdown">
  {#if label}<span class="lbl">{label}</span>{/if}
  <select {value} {disabled} onchange={onChange}>
    {#each opts as o}
      <option value={o.value} selected={o.value === value}>{o.label}</option>
    {/each}
  </select>
</label>

<style>
  .xi-dropdown { display: inline-flex; align-items: center; gap: 0.5rem; font: 13px system-ui, sans-serif; color: var(--xi-fg, inherit); }
  select { padding: 0.15rem 0.3rem; }
</style>
