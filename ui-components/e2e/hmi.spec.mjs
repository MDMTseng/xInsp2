// hmi.spec.mjs — the HMI in a real browser, talking to a live backend (test 1).
// Serves hmi/ and points it at a backend running the demo project (autostream),
// via the HMI's ?ws= override. Verifies: connect, dashboard cards render, live
// data flows (value card shows a number), and the image card is now backed by
// the shared <xi-image-viewer>.
import { test, expect } from "@playwright/test";
import { resolve, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { serveStatic, startBackend, REPO } from "./_serve.mjs";

const HMI = resolve(dirname(fileURLToPath(import.meta.url)), "../../hmi");

let backend, site;
test.beforeAll(async () => {
  backend = await startBackend({ project: join(REPO, "hmi", "demo"), fps: 6 });
  site = await serveStatic(HMI);
});
test.afterAll(async () => { backend?.stop(); await site?.close(); });

test("HMI connects, renders the dashboard, streams live data into cards", async ({ page }) => {
  await page.goto(`${site.url}/?ws=ws://127.0.0.1:${backend.port}/`);

  // Connected (the indicator stops saying "connecting…").
  await expect(page.locator("#conn")).not.toHaveText(/connecting/i, { timeout: 25_000 });

  // The dashboard renders cards.
  const cards = page.locator("#grid xi-card-value, #grid xi-card-image, #grid xi-card-verdict, #grid xi-card-spc, #grid xi-card-throughput, #grid xi-card-yield");
  await expect(cards.first()).toBeVisible({ timeout: 15_000 });

  // The image card is backed by the shared xi-image-viewer (adoption, #80).
  await expect(page.locator("#grid xi-card-image xi-image-viewer")).toHaveCount(1, { timeout: 15_000 });

  // Live data: the demo streams fg_pct (a number) → the value card leaves "—".
  const valueCard = page.locator("#grid xi-card-value").first();
  await expect.poll(async () => (await valueCard.innerText()).trim(), { timeout: 25_000, intervals: [500] })
    .not.toBe("—");
});
