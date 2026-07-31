<svelte:options
  customElement={{
    tag: "xi-file",
    props: { value: { reflect: true, type: "String" } },
  }}
/>

<script>
  // A path (model, reference image, recipe). A webview cannot open a native file
  // dialog itself, so Browse EMITS `browse` and the HOST answers by setting
  // `value` — the same split as the numpad: the widget declares the intent, the
  // host owns the picker. Typing a path directly still works.
  let { value = $bindable(""), label = "", disabled = false, accept = "" } = $props();
  const host = $host();
  const fire = (t, detail) => host.dispatchEvent(
    new CustomEvent(t, { detail, bubbles: true, composed: true }));
  function onChange(e) { value = e.target.value; fire("change", { value }); }
  function browse() { if (!disabled) fire("browse", { value, accept }); }
</script>

<label class="xi-file">
  {#if label}<span class="lbl">{label}</span>{/if}
  <span class="wrap">
    <input type="text" {value} {disabled} placeholder="path…" onchange={onChange} />
    <button type="button" {disabled} onclick={browse}>Browse…</button>
  </span>
</label>

<style>
  .xi-file { display: inline-flex; align-items: center; gap: 0.5rem;
    font: var(--xi-font, 13px system-ui, sans-serif); color: var(--xi-fg, inherit); }
  .wrap { display: inline-flex; align-items: stretch; gap: 0.3rem; min-width: 0; }
  input {
    flex: 1; min-width: 0; padding: 0.15rem 0.3rem;
    color: var(--xi-fg, inherit); background: var(--xi-bg, #fff);
    border: 1px solid var(--xi-border, #ccc); border-radius: var(--xi-radius, 3px);
  }
  input:focus { outline: 1px solid var(--xi-accent, #3b82f6); outline-offset: -1px; }
  button {
    min-height: 2.2rem; padding: 0 0.7rem; cursor: pointer;
    color: var(--xi-fg, inherit); background: var(--xi-bg, #fff);
    border: 1px solid var(--xi-border, #ccc); border-radius: var(--xi-radius, 3px);
  }
  button:disabled { opacity: 0.5; cursor: default; }
</style>
