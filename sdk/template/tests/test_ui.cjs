//
// test_ui.cjs — UI E2E test for my_plugin.
//
// Drives the full stack: VS Code commands, the live plugin webview, the
// running backend. Receives `h` — see README "UI / E2E tests" for the
// full helpers cheatsheet.
//
// Run via:
//   node <xinsp2>/sdk/testing/run_ui_test.mjs <this-plugin-folder>     (cold session)
//   VS Code → "xInsp2: Run Plugin UI Tests"                            (warm session)
//
// Screenshots land in tests/screenshots/.
//

module.exports = {
    async run(h) {
        const projDir = h.tmp();

        // 1. Project + instance via the user-facing commands
        await h.createProject(projDir, 'my_plugin_demo');
        await h.addInstance('inst0', 'my_plugin');

        // 2. Open the webview, screenshot the empty state
        await h.openUI('inst0', 'my_plugin');
        h.shot('a_ui_opened');

        // 3. Type into the threshold input, toggle invert, click Apply
        h.setInput('inst0', '#threshold', 200);
        await h.sleep(150);
        h.click('inst0', '#invert');
        await h.sleep(150);
        h.click('inst0', 'button[onclick="apply()"]');
        await h.sleep(400);
        h.shot('b_after_apply');

        // 4. Round-trip status — UI clicks should have updated state
        await h.getStatus('inst0');
        h.expect(h.lastStatus !== null, 'lastStatus populated');
        h.expectEq(h.lastStatus.threshold, 200, 'threshold reflects UI input');
        h.expectEq(h.lastStatus.invert, true,    'invert reflects checkbox');
        h.expect(typeof h.lastStatus.folder === 'string' && h.lastStatus.folder.length > 0,
                 'folder_path() reports the instance folder');

        // 5. Reset clears everything back to defaults
        h.click('inst0', 'button[onclick="reset()"]');
        await h.sleep(400);
        await h.getStatus('inst0');
        h.expectEq(h.lastStatus.threshold, 128, 'reset → threshold default');
        h.expectEq(h.lastStatus.invert, false,  'reset → invert default');
        h.shot('c_after_reset');
    },
};
