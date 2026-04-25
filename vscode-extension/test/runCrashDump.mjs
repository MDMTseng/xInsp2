// runCrashDump.mjs — backend `--test-crash` raises an uncaught
// exception; verify SetUnhandledExceptionFilter writes a minidump.
//
import { spawnSync } from 'node:child_process';
import { existsSync, readdirSync, mkdirSync, rmSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const exe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');
const dumpDir = resolve(tmpdir(), 'xinsp2', 'crashdumps');

// Snapshot existing dumps so we only count NEW ones.
mkdirSync(dumpDir, { recursive: true });
const before = new Set(readdirSync(dumpDir).filter(f => f.endsWith('.dmp')));
console.log(`existing dumps: ${before.size}`);

console.log(`triggering --test-crash on ${exe}`);
const r = spawnSync(exe, ['--test-crash'], { encoding: 'utf8', timeout: 10000 });
console.log(`  exit code: ${r.status}`);
console.log(`  stderr: ${(r.stderr || '').split('\n').slice(-3).join(' | ').trim()}`);

let failed = 0;
function check(c, label) { if (c) console.log(`  ✓ ${label}`); else { console.log(`  ✗ ${label}`); failed++; } }

check(r.status !== 0, `process exited non-zero (${r.status})`);
const stderr = r.stderr || '';
check(stderr.includes('CRASH') && stderr.includes('minidump'),
    'stderr mentions CRASH + minidump path');

// Find the new .dmp file
const after = readdirSync(dumpDir).filter(f => f.endsWith('.dmp'));
const fresh = after.filter(f => !before.has(f));
console.log(`  new dumps: ${fresh.length}`);
check(fresh.length >= 1, 'at least 1 new minidump written');
if (fresh.length > 0) {
    const f = resolve(dumpDir, fresh[0]);
    const sz = statSync(f).size;
    console.log(`  newest: ${fresh[0]}  (${(sz/1024).toFixed(1)} KB)`);
    check(sz > 1024, `minidump non-trivial size (${sz} bytes)`);
    check(sz < 50 * 1024 * 1024, `minidump within reason (< 50 MB, got ${(sz/1024/1024).toFixed(1)} MB)`);
}

if (failed > 0) { console.error(`\nFAIL: ${failed} check(s) failed`); process.exit(1); }
console.log('\nOK: top-level minidump filter writes a usable .dmp');
