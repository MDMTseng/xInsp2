<svelte:options
  customElement={{
    tag: "xi-button",
    props: {
      secondary: { reflect: true, type: "Boolean" },
      disabled: { reflect: true, type: "Boolean" },
      icon: {},
    },
  }}
/>

<script>
  // Action button. No custom event: the inner <button>'s native click is
  // `composed`, so it crosses the shadow boundary and retargets to the host —
  // host-level `addEventListener("click", …)` and inline `onclick` fire the same
  // as a plain <button>. `secondary` reflects so callers can toggle the
  // attribute to switch emphasis. `icon` maps a few names to glyphs (the kit
  // carries no icon font).
  let { secondary = false, disabled = false, icon = "" } = $props();

  const GLYPH = { add: "＋", play: "▶", "debug-stop": "■", stop: "■" };
  const glyph = $derived(icon ? (GLYPH[icon] ?? "") : "");
</script>

<button class="xi-button" class:secondary {disabled}>
  {#if glyph}<span class="ico" aria-hidden="true">{glyph}</span>{/if}
  <slot />
</button>

<style>
  .xi-button {
    display: inline-flex; align-items: center; gap: 0.4em;
    font: var(--xi-font, 13px system-ui, sans-serif);
    padding: 0.35em 0.9em;
    border: 1px solid transparent;
    border-radius: var(--xi-radius, 3px);
    background: var(--xi-btn-bg, #3b82f6);
    color: var(--xi-btn-fg, #fff);
    cursor: pointer;
  }
  .xi-button:hover { background: var(--xi-btn-hover-bg, #2f6fe0); }
  .xi-button:focus-visible { outline: 1px solid var(--xi-accent, #3b82f6); outline-offset: 2px; }
  .xi-button.secondary {
    background: var(--xi-btn-secondary-bg, #444);
    color: var(--xi-btn-secondary-fg, #fff);
  }
  .xi-button.secondary:hover { background: var(--xi-btn-secondary-hover-bg, #4f4f4f); }
  .xi-button:disabled { opacity: 0.5; cursor: default; }
  .ico { font-size: 0.9em; line-height: 1; }
</style>
