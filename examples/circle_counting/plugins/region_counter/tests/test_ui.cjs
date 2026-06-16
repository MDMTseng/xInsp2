//
// test_ui.cjs — UI E2E test for region_counter.
//
// Drives the param-tuning path THROUGH THE INSTANCE UI (the VS Code webview
// DOM): moves the close/min/max range sliders, clicks Apply, and asserts the
// backend's param state round-trips back to the UI values. Then Reset → defaults.
//
// Uses the convention-based helpers (docs/guides/write-a-plugin.md):
//   h.useProjectPlugin(dir) — compile this source-only plugin as a project plugin
//   h.setParam(inst, '<param_name>', v) — targets [data-param="<name>"]
//   h.action(inst, '<action>')          — targets [data-action="<name>"]
// (the older h.setInput('#close', v) / h.click('#apply') id-targeting still works.)
//
// Run via:
//   node <xinsp2>/sdk/testing/run_ui_test.mjs examples/circle_counting/plugins/region_counter
// Screenshots land in tests/screenshots/.
//
// DOM contract (ui/index.html): data-param close_radius/min_area/max_area on the
// range inputs; data-action apply/reset on the buttons.
// defaults: close_radius=2, min_area=300, max_area=4000
//

module.exports = {
    async run(h) {
        const inst   = 'inst0';
        const plugin = 'region_counter';
        const projDir = h.tmp();

        // 1. Create project, then compile region_counter as a project plugin.
        //    (The scanned external folder ships source but no built .dll/cert, so
        //    a plain createProject+addInstance fails the cert gate — useProjectPlugin
        //    copies the source in + reopens so the backend compiles it from src/.)
        await h.createProject(projDir, 'region_counter_demo');
        await h.useProjectPlugin(projDir, plugin);
        await h.addInstance(inst, plugin);

        // 2. Open the webview, let it mount + fetch initial status.
        await h.openUI(inst, plugin);
        await h.sleep(800);
        h.shot('ui_opened');

        await h.getStatus(inst);
        h.expect(h.lastStatus !== null, 'initial lastStatus populated');
        if (h.lastStatus) {
            console.log(`  initial state: close=${h.lastStatus.close_radius} ` +
                        `min=${h.lastStatus.min_area} max=${h.lastStatus.max_area}`);
        }

        // 3. Adjust each param via the DOM, BY PARAM NAME (real input/change events).
        h.setParam(inst, 'close_radius', 8);
        await h.sleep(250);
        h.shot('set_close');

        h.setParam(inst, 'min_area', 1200);
        await h.sleep(250);
        h.shot('set_min');

        h.setParam(inst, 'max_area', 9000);
        await h.sleep(250);
        h.shot('set_max');

        // 4. Apply — the webview posts set_close_radius/set_min_area/set_max_area.
        h.action(inst, 'apply');
        await h.sleep(600);
        h.shot('applied');

        // Prove the UI -> backend round-trip.
        await h.getStatus(inst);
        h.expect(h.lastStatus !== null, 'lastStatus populated after apply');
        h.expectEq(h.lastStatus.close_radius, 8,    'close_radius reflects UI close_radius=8');
        h.expectEq(h.lastStatus.min_area,     1200, 'min_area reflects UI min_area=1200');
        h.expectEq(h.lastStatus.max_area,     9000, 'max_area reflects UI max_area=9000');

        // 5. Reset — params return to defaults.
        h.action(inst, 'reset');
        await h.sleep(600);
        h.shot('reset');

        await h.getStatus(inst);
        h.expectEq(h.lastStatus.close_radius, 2,    'reset -> close_radius default 2');
        h.expectEq(h.lastStatus.min_area,     300,  'reset -> min_area default 300');
        h.expectEq(h.lastStatus.max_area,     4000, 'reset -> max_area default 4000');

        // 6. Final readout screenshot.
        await h.sleep(300);
        h.shot('final_status');
    },
};
