<svelte:options
  customElement={{
    tag: "xi-badge",
    props: { variant: { reflect: true } },
  }}
/>

<script>
  // Small status pill. Text comes from the host's light DOM (`el.textContent =
  // …`) projected through <slot>, so callers update it exactly like the
  // web-component it replaces. `variant="counter"` renders a rounded counter.
  let { variant = "" } = $props();
</script>

<span class="xi-badge" class:counter={variant === "counter"}>
  <slot />
</span>

<style>
  .xi-badge {
    display: inline-flex; align-items: center;
    font: var(--xi-font, 11px system-ui, sans-serif);
    font-size: 0.85em; line-height: 1;
    padding: 0.2em 0.55em;
    border-radius: var(--xi-radius, 3px);
    background: var(--xi-badge-bg, #4d4d4d);
    color: var(--xi-badge-fg, #fff);
    white-space: nowrap;
  }
  .xi-badge.counter { border-radius: 999px; padding: 0.2em 0.6em; }
</style>
