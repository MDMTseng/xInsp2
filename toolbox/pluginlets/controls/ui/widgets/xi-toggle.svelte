<svelte:options
  customElement={{
    tag: "xi-toggle",
    props: { value: { reflect: true, type: "Boolean" } },
  }}
/>

<script>
  // Key-bound boolean. `value` is the boolean state; emits `change` with {value}.
  // Uniform with the other controls (every widget exposes `value`) so the
  // auto-renderer wires key→value the same way.
  let { value = $bindable(false), label = "", disabled = false } = $props();
  const host = $host();
  function onChange(e) {
    value = e.target.checked;
    host.dispatchEvent(new CustomEvent("change", { detail: { value }, bubbles: true, composed: true }));
  }
</script>

<label class="xi-toggle">
  <input type="checkbox" checked={value} {disabled} onchange={onChange} />
  {#if label}<span class="lbl">{label}</span>{/if}
</label>

<style>
  .xi-toggle { display: inline-flex; align-items: center; gap: 0.4rem; font: 13px system-ui, sans-serif; color: var(--xi-fg, inherit); cursor: pointer; }
  input { accent-color: var(--xi-accent, #3b82f6); }
</style>
