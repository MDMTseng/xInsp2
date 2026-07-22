<svelte:options
  customElement={{
    tag: "xi-stepper",
    props: {
      value: { reflect: true, type: "Number" },
      min: { type: "Number" },
      max: { type: "Number" },
      step: { type: "Number" },
    },
  }}
/>

<script>
  // Bounded numeric with -/+ increment buttons — the touch-precise counterpart of
  // a slider for COUNTS (caliper count, N), where dragging is fiddly. The buttons
  // are deliberately large hit targets for kiosk/touch use. Emits `change`.
  import { openNumpad } from "../lib/numpad.mjs";
  let { value = $bindable(0), min, max, step = 1, label = "", disabled = false,
        numpad = true } = $props();
  const host = $host();
  const fire = () => host.dispatchEvent(
    new CustomEvent("change", { detail: { value }, bubbles: true, composed: true }));

  const clamp = (v) => {
    if (min != null && v < min) v = min;
    if (max != null && v > max) v = max;
    return v;
  };
  // Keep the value on the step grid relative to min, so +/- never drifts.
  const snap = (v) => {
    const s = Number(step) || 1;
    const base = min != null ? Number(min) : 0;
    return clamp(base + Math.round((v - base) / s) * s);
  };
  function bump(dir) {
    if (disabled) return;
    value = snap(Number(value || 0) + dir * (Number(step) || 1));
    fire();
  }
  function onEdit(e) {
    const v = e.target.value === "" ? null : Number(e.target.value);
    value = v == null ? v : snap(v);
    fire();
  }
  // Touch entry: the host-owned numpad surface (see ../lib/numpad.mjs).
  function tapNumpad() {
    if (disabled || !numpad) return;
    openNumpad({
      value, min, max, label,
      onCommit: (v) => { value = snap(Number(v)); fire(); },
    });
  }
</script>

<div class="xi-stepper">
  {#if label}<span class="lbl">{label}</span>{/if}
  <div class="grp">
    <button type="button" class="pm" {disabled} onclick={() => bump(-1)} aria-label="decrement">−</button>
    <input type="number" {min} {max} {step} {value} {disabled}
           onchange={onEdit} onfocus={tapNumpad} />
    <button type="button" class="pm" {disabled} onclick={() => bump(1)} aria-label="increment">+</button>
  </div>
</div>

<style>
  .xi-stepper { display: inline-flex; align-items: center; gap: 0.5rem;
    font: var(--xi-font, 13px system-ui, sans-serif); color: var(--xi-fg, inherit); }
  .grp { display: inline-flex; align-items: stretch; }
  .pm {
    /* big hit targets: kiosk/touch first */
    min-width: 2.2rem; min-height: 2.2rem; font-size: 1.1rem; line-height: 1;
    color: var(--xi-fg, inherit);
    background: var(--xi-bg, #fff);
    border: 1px solid var(--xi-border, #ccc);
    cursor: pointer;
  }
  .pm:first-child { border-radius: var(--xi-radius, 3px) 0 0 var(--xi-radius, 3px); }
  .pm:last-child  { border-radius: 0 var(--xi-radius, 3px) var(--xi-radius, 3px) 0; }
  .pm:disabled { opacity: 0.5; cursor: default; }
  input {
    width: 4.5em; text-align: center; padding: 0.15rem 0.3rem;
    color: var(--xi-fg, inherit);
    background: var(--xi-bg, #fff);
    border: 1px solid var(--xi-border, #ccc);
    border-left: 0; border-right: 0;
    accent-color: var(--xi-accent, #3b82f6);
    -moz-appearance: textfield;
  }
  input::-webkit-outer-spin-button, input::-webkit-inner-spin-button {
    -webkit-appearance: none; margin: 0;   /* our own +/- replace the native spinners */
  }
  input:focus { outline: 1px solid var(--xi-accent, #3b82f6); outline-offset: -1px; }
</style>
