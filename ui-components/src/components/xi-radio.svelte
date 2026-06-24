<svelte:options
  customElement={{
    tag: "xi-radio",
    props: { value: { reflect: true }, options: {} },
  }}
/>

<script>
  // Key-bound radio group. `options` is an array (JS property) or JSON string
  // attribute of values / {value,label}. Exposes `value`; emits `change`.
  import { parseOptions } from "../lib/options.mjs";
  let { value = $bindable(""), options = [], label = "", disabled = false, name = "xi-radio" } = $props();
  const host = $host();
  const opts = $derived(parseOptions(options));
  function pick(v) {
    value = v;
    host.dispatchEvent(new CustomEvent("change", { detail: { value }, bubbles: true, composed: true }));
  }
</script>

<div class="xi-radio" role="radiogroup">
  {#if label}<span class="lbl">{label}</span>{/if}
  {#each opts as o}
    <label class="opt">
      <input type="radio" {name} value={o.value} checked={o.value === value} {disabled}
             onchange={() => pick(o.value)} />
      <span>{o.label}</span>
    </label>
  {/each}
</div>

<style>
  .xi-radio { display: inline-flex; align-items: center; gap: 0.75rem; font: 13px system-ui, sans-serif; color: var(--xi-fg, inherit); }
  .opt { display: inline-flex; align-items: center; gap: 0.25rem; cursor: pointer; }
  input { accent-color: var(--xi-accent, #3b82f6); }
</style>
