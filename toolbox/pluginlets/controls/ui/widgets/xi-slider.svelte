<svelte:options
  customElement={{
    tag: "xi-slider",
    props: {
      value: { reflect: true, type: "Number" },
      min: { type: "Number" },
      max: { type: "Number" },
      step: { type: "Number" },
    },
  }}
/>

<script>
  // A key-bound range control. Pure UI: it exposes `value` (a property the host
  // reads/writes) and emits `input` (live) + `change` (committed) custom events.
  // Wiring `change` → set_instance_def is the consumer's / auto-renderer's job —
  // this component stays decoupled from the WS layer (tap-out, not iframe).
  let {
    value = $bindable(0),
    min = 0,
    max = 100,
    step = 1,
    label = "",
    disabled = false,
  } = $props();

  const host = $host();
  const fire = (type) =>
    host.dispatchEvent(new CustomEvent(type, { detail: { value }, bubbles: true, composed: true }));

  function onInput(e) { value = Number(e.target.value); fire("input"); }
  function onChange(e) { value = Number(e.target.value); fire("change"); }
</script>

<label class="xi-slider">
  {#if label}<span class="lbl">{label}</span>{/if}
  <input type="range" {min} {max} {step} {value} {disabled} oninput={onInput} onchange={onChange} />
  <span class="val">{value}</span>
</label>

<style>
  .xi-slider {
    display: inline-flex;
    align-items: center;
    gap: 0.5rem;
    font: 13px system-ui, sans-serif;
    color: var(--xi-fg, inherit);
  }
  input[type="range"] { accent-color: var(--xi-accent, #3b82f6); vertical-align: middle; }
  .val { min-width: 2.5em; text-align: right; font-variant-numeric: tabular-nums; }
</style>
