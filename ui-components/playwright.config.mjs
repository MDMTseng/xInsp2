import { defineConfig, devices } from "@playwright/test";

// Browser e2e — verifies the parts node/jsdom can't: real canvas render, drag/
// zoom, pixel probe, live WS round-trips. Each spec spawns its own backend +
// static server (see e2e/_serve.mjs). Chromium only; headless.
export default defineConfig({
  testDir: "e2e",
  fullyParallel: false,
  workers: 1,
  timeout: 60_000,
  use: { ...devices["Desktop Chrome"], headless: true },
  projects: [{ name: "chromium" }],
});
