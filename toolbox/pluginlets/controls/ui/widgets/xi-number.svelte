<svelte:options
  customElement={{
    tag: "xi-number",
    props: {
      value: { reflect: true, type: "Number" },
      min: { type: "Number" },
      max: { type: "Number" },
      step: { type: "Number" },
    },
  }}
/>

<script>
  // Key-bound numeric input. Exposes `value` + emits `input`/`change` (tap-out).
  let { value = $bindable(0), min, max, step = 1, label = "", disabled = false } = $props();
  const host = $host();
  const fire = (t) => host.dispatchEvent(new CustomEvent(t, { detail: { value }, bubbles: true, composed: true }));
  const read = (e) => (e.target.value === "" ? null : Number(e.target.value));
  function onInput(e) { value = read(e); fire("input"); }
  function onChange(e) { value = read(e); fire("change"); }
</script>

<label class="xi-number">
  {#if label}<span class="lbl">{label}</span>{/if}
  <input type="number" {min} {max} {step} {value} {disabled} oninput={onInput} onchange={onChange} />
</label>

<style>
  .xi-number { display: inline-flex; align-items: center; gap: 0.5rem; font: var(--xi-font, 13px system-ui, sans-serif); color: var(--xi-fg, inherit); }
  input {
    width: 6em; padding: 0.15rem 0.3rem;
    color: var(--xi-fg, inherit);
    background: var(--xi-bg, #fff);
    border: 1px solid var(--xi-border, #ccc);
    border-radius: var(--xi-radius, 3px);
    accent-color: var(--xi-accent, #3b82f6);
  }
  input:focus { outline: 1px solid var(--xi-accent, #3b82f6); outline-offset: -1px; }
</style>
