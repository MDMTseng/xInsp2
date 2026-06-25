// webapp.spec.mjs — a vanilla page that LIBRARY-IMPORTS the components, in a real
// browser. Verifies what jsdom can't: the WS round-trip, the slider's
// input→change→set_instance_def chain, and the viewer's canvas render + pixel
// probe. Serves ui-components/ and points demo/poc.html at a live backend.
import { test, expect } from "@playwright/test";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { serveStatic, startBackend } from "./_serve.mjs";

const UI = resolve(dirname(fileURLToPath(import.meta.url)), "..");

let backend, site;
test.beforeAll(async () => { backend = await startBackend(); site = await serveStatic(UI); });
test.afterAll(async () => { backend?.stop(); await site?.close(); });

test("poc.html: connect, slider→set_instance_def round-trip, viewer frame + pixel probe", async ({ page }) => {
  const log = page.locator("#log");
  await page.goto(`${site.url}/demo/poc.html`);

  // Point at the test backend + connect.
  await page.fill("#url", `ws://127.0.0.1:${backend.port}/`);
  await page.click("#connect");
  await expect(log).toContainText("connected", { timeout: 20_000 });
  await expect(log).toContainText("bound xi-slider", { timeout: 20_000 });

  // Drive the REAL slider input → its change event → poc wires set_instance_def →
  // reads the def back. Full chain in a real browser (no jsdom delegation gap).
  await page.evaluate(() => {
    const inp = document.getElementById("s").shadowRoot.querySelector('input[type="range"]');
    inp.value = "200";
    inp.dispatchEvent(new Event("change", { bubbles: true }));
  });
  await expect(log).toContainText("read back value=200", { timeout: 10_000 });

  // Feed a synthetic frame, then click the viewer → pixel probe fires with rgb.
  await page.click("#gen");
  await expect(log).toContainText("fed synthetic", { timeout: 10_000 });
  await page.waitForTimeout(200); // let the image decode + draw
  await page.locator("#viewer").click({ position: { x: 120, y: 90 } });
  await expect(log).toContainText("pixelpick", { timeout: 10_000 });
});
