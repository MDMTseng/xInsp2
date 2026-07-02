<svelte:options
  customElement={{
    tag: "xi-text",
    props: { value: { reflect: true }, placeholder: {} },
  }}
/>

<script>
  // Free-text input. Exposes `value` (so `el.value` reads/writes the string like
  // a native field) and emits `input`/`change`, uniform with the other controls.
  let { value = $bindable(""), placeholder = "", disabled = false } = $props();
  const host = $host();
  const fire = (t) => host.dispatchEvent(new CustomEvent(t, { detail: { value }, bubbles: true, composed: true }));
  function onInput(e) { value = e.target.value; fire("input"); }
  function onChange(e) { value = e.target.value; fire("change"); }
</script>

<input class="xi-text" type="text" {value} {placeholder} {disabled}
       oninput={onInput} onchange={onChange} />

<style>
  .xi-text {
    box-sizing: border-box; width: 100%;
    font: var(--xi-font, 13px system-ui, sans-serif);
    padding: 0.3em 0.5em;
    color: var(--xi-fg, inherit);
    background: var(--xi-bg, #fff);
    border: 1px solid var(--xi-border, #ccc);
    border-radius: var(--xi-radius, 3px);
  }
  .xi-text:focus { outline: 1px solid var(--xi-accent, #3b82f6); outline-offset: -1px; }
</style>
