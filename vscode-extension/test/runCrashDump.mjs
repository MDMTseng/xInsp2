// runCrashDump.mjs — verify the full crash → report → recovery path:
//   1. Backend with --test-crash raises an uncaught exception.
//   2. Top-level filter writes both a .dmp AND a JSON sidecar with
//      exception kind, faulting module, and last-activity context.
//   3. On next startup, the new backend's cmd:crash_reports surfaces
//      the saved JSON for the extension to display.
//   4. cmd:clear_crash_reports cleans up.
//
import { spawn, spawnSync } from 'node:child_process';
import { existsSync, readdirSync, mkdirSync, statSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const exe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');
const dumpDir = resolve(tmpdir(), 'xinsp2', 'crashdumps');

mkdirSync(dumpDir, { recursive: true });
const beforeJson = new Set(readdirSync(dumpDir).filter(f => f.endsWith('.json')));
const beforeDmp  = new Set(readdirSync(dumpDir).filter(f => f.endsWith('.dmp')));

let failed = 0;
function check(c, label) { if (c) console.log(`  ✓ ${label}`); else { console.log(`  ✗ ${label}`); failed++; } }

// --- 1. Trigger crash ----------------------------------------------------
console.log(`-- triggering --test-crash --`);
const r = spawnSync(exe, ['--test-crash'], { encoding: 'utf8', timeout: 10000 });
console.log(`  exit code: ${r.status}`);
console.log(`  stderr: ${(r.stderr || '').split('\n').slice(-3).join(' | ').trim()}`);
check(r.status !== 0, 'process exited non-zero');
check((r.stderr || '').includes('CRASH'), 'stderr mentions CRASH');

// --- 2. Inspect new artifacts -------------------------------------------
const newDmp  = readdirSync(dumpDir).filter(f => f.endsWith('.dmp')  && !beforeDmp.has(f));
const newJson = readdirSync(dumpDir).filter(f => f.endsWith('.json') && !beforeJson.has(f));
console.log(`-- new artifacts: ${newDmp.length} .dmp, ${newJson.length} .json --`);
check(newDmp.length  >= 1, 'minidump written');
check(newJson.length >= 1, 'JSON sidecar written');

if (newJson.length > 0) {
    const f = resolve(dumpDir, newJson[0]);
    const body = readFileSync(f, 'utf8');
    let report;
    try { report = JSON.parse(body); } catch (e) { console.log('  invalid JSON: ' + e.message); failed++; }
    if (report) {
        console.log(`  exception:    ${report.exception?.code} (${report.exception?.name}) in ${report.exception?.module}`);
        console.log(`  context:      cmd=${report.context?.last_cmd}  script=${report.context?.last_script}`);
        check(report.version,                     'report.version present');
        check(report.exception?.code === '0xE0000001', `exception.code = 0xE0000001 (got ${report.exception?.code})`);
        // Module blame: synthetic RaiseException trips inside KERNELBASE
        // (the OS-side raise-implementation) — that's correct. For real
        // segfaults in user code the blame would point at the offending
        // DLL. We just check it resolved to a known module, not "<unknown>".
        check(typeof report.exception?.module === 'string'
              && report.exception.module !== '<unknown>'
              && report.exception.module.includes('+0x'),
              `module blame resolved to a loaded DLL (got "${report.exception?.module}")`);
        check(typeof report.exception?.address === 'string', 'exception.address present');
        check(report.minidump?.endsWith('.dmp'),  'minidump filename present');
        check(typeof report.context?.last_cmd === 'string', 'context.last_cmd present');
    }
}

// --- 3. Bring backend back up + cmd:crash_reports -----------------------
console.log(`\n-- starting fresh backend, querying crash_reports --`);
const port = 40000 + Math.floor(Math.random() * 20000);
const be = spawn(exe, [`--port=${port}`], { stdio: ['ignore', 'pipe', 'pipe'] });
be.stderr.on('data', d => process.stderr.write(`[be] ${d}`));
await new Promise(r => setTimeout(r, 2200));

const ws = new WebSocket(`ws://127.0.0.1:${port}`);
await new Promise((res, rej) => { ws.once('open', res); ws.once('error', rej); });
const handlers = new Map();
ws.on('message', (d, isBin) => {
    if (isBin) return;
    const t = d.toString();
    if (t[0] !== '{') return;
    try { const m = JSON.parse(t);
        if (m.type === 'rsp' && handlers.has(m.id)) { handlers.get(m.id)(m); handlers.delete(m.id); }
    } catch {}
});
const send = (name, args) => new Promise(res => {
    const id = Math.floor(Math.random() * 1e9);
    handlers.set(id, res);
    ws.send(JSON.stringify({ type: 'cmd', id, name, args }));
});

const cr = await send('crash_reports');
check(cr.ok, 'crash_reports cmd ok');
const reports = cr.data?.reports || [];
console.log(`  reports surfaced: ${reports.length}`);
check(reports.length >= 1, 'recovered ≥ 1 crash report');
if (reports.length > 0) {
    const r0 = reports[0];
    check(typeof r0.file === 'string' && r0.file.endsWith('.json'), 'report has .file name');
    check(r0.report?.exception?.code === '0xE0000001', 'first report (newest) is our test crash');
}

// Cleanup
const cleared = await send('clear_crash_reports');
check(cleared.ok, 'clear_crash_reports ok');
console.log(`  cleared: ${cleared.data?.removed} files`);

const cr2 = await send('crash_reports');
check((cr2.data?.reports || []).length === 0, 'reports list empty after clear');

ws.close();
be.kill();
await new Promise(r => setTimeout(r, 400));

if (failed > 0) { console.error(`\nFAIL: ${failed} check(s) failed`); process.exit(1); }
console.log('\nOK: crash → JSON sidecar → cmd:crash_reports → cmd:clear_crash_reports');
