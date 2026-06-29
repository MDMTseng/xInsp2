// ws_openmp.test.mjs — opt-in OpenMP for the script path.
//
// A project.json with "openmp": true makes the script compile with /openmp, so
// `#pragma omp` actually parallelizes. We prove the flag took effect three ways:
//   - the script #includes <omp.h> and calls omp_get_max_threads() — that only
//     LINKS when /openmp is passed (vcomp140.dll), so a successful compile is
//     itself proof;
//   - the `_OPENMP` macro is defined only under /openmp;
//   - a parallel-for reduction returns the correct sum.

import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { withBackend } from './helpers/client.mjs';

const slash = (s) => s.split('\\').join('/');

const CAP = 4;
const PROJECT_JSON = JSON.stringify({
    name: 'omp_probe', script: 'inspect.cpp', params: [], instances: [], openmp_max_threads: CAP,
}, null, 2);

const SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <omp.h>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int){
#ifdef _OPENMP
  const int omp_macro = 1;
#else
  const int omp_macro = 0;
#endif
  VAR(omp_enabled, omp_macro);            // 1 only if /openmp was passed
  VAR(max_threads, omp_get_max_threads()); // links only under /openmp

  // distinct threads the runtime actually spins up
  int used = 0;
  #pragma omp parallel
  {
    #pragma omp single
    used = omp_get_num_threads();
  }
  VAR(used_threads, used);

  // parallel-for reduction: sum 0..N-1 == N*(N-1)/2
  const int N = 1000000;
  long long sum = 0;
  #pragma omp parallel for reduction(+:sum)
  for (int i = 0; i < N; ++i) sum += i;
  VAR(sum_ok, (sum == 499999500000LL) ? 1 : 0);
}`;

async function waitRsp(c, id) { for (;;) { const m = await c.nextText(); if (m.type === 'rsp' && m.id === id) return m; } }

test('OpenMP opt-in (project.json "openmp_max_threads") compiles + caps threads', { skip: 'vars/preview/subscribe removed with VAR (branch refactor/remove-var-core) — pending preview plugin (observed via vars)' }, async () => {
    const dir = mkdtempSync(join(tmpdir(), 'xi_omp_'));
    writeFileSync(join(dir, 'project.json'), PROJECT_JSON);
    const sp = join(dir, 'inspect.cpp');
    writeFileSync(sp, SCRIPT);
    await withBackend(async (c) => {
        await c.nextText();
        c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(dir) } });
        assert.equal((await waitRsp(c, 1)).ok, true, 'open_project ok');
        // optimize:true so the compile path mirrors production (and isn't /Od)
        c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: slash(sp), optimize: true } });
        const r = await waitRsp(c, 2);
        assert.equal(r.ok, true, 'compile ok — /openmp linked vcomp140.dll (proves the flag)');
        c.send({ type: 'cmd', id: 3, name: 'run' });
        await waitRsp(c, 3);
        let vars = null;
        for (;;) { const m = await c.nextText(); if (m.type === 'vars') { vars = m.items; break; } }
        const v = Object.fromEntries(vars.map(x => [x.name, x.value]));
        console.log('vars:', JSON.stringify(v));
        assert.equal(v.omp_enabled, 1, '_OPENMP defined → /openmp was passed');
        assert.equal(v.max_threads, CAP, 'cap applied at load → omp_get_max_threads == openmp_max_threads');
        assert.ok(v.used_threads >= 1 && v.used_threads <= CAP, 'parallel region ran within the cap');
        assert.equal(v.sum_ok, 1, 'parallel-for reduction is correct');
    });
});
