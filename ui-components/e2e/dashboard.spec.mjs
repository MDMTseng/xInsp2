// dashboard.spec.mjs — an EXTERNAL webapp imports the whole HMI dashboard
// (mountDashboard) from the library and renders it live (task #81). This is the
// concrete answer to "can an external app import the HMI + layout?". Serves
// ui-components/ + a backend on the demo project (autostream).
import { test, expect } from "@playwright/test";
import { resolve, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { serveStatic, startBackend, REPO } from "./_serve.mjs";

const UI = resolve(dirname(fileURLToPath(import.meta.url)), "..");

let backend, site;
test.beforeAll(async () => {
  backend = await startBackend({ project: join(REPO, "hmi", "demo"), fps: 6 });
  site = await serveStatic(UI);
});
test.afterAll(async () => { backend?.stop(); await site?.close(); });

test("external page: import mountDashboard → live HMI dashboard", async ({ page }) => {
  await page.goto(`${site.url}/demo/dashboard.html?ws=ws://127.0.0.1:${backend.port}/`);

  await expect(page.locator("#status")).toHaveText("connected", { timeout: 25_000 });

  // The dashboard rendered HMI cards…
  await expect(page.locator("#dash xi-card-value")).toBeVisible({ timeout: 15_000 });
  // …with the image card backed by the shared viewer (one library, top to bottom).
  await expect(page.locator("#dash xi-card-image xi-image-viewer")).toHaveCount(1, { timeout: 15_000 });

  // …and live data flows from the backend stream into the value card.
  const value = page.locator("#dash xi-card-value").first();
  await expect.poll(async () => (await value.innerText()).trim(), { timeout: 25_000, intervals: [500] }).not.toBe("—");
});
