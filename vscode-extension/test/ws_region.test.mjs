// ws_region.test.mjs — Region: a nominal type whose payload is a binary MASK
// image (image channel), not cJSON. Builds a mask, wraps it as a Region, reads
// dims/area/bbox via the cv helpers, round-trips through cv::Mat, and checks NA.

import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { withBackend } from './helpers/client.mjs';

const slash = (s) => s.split('\\').join('/');
const SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_types.hpp>
#include <xi/xi_types_cv.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int){
  // 10x8 gray image; paint a 4x3 block at (x=2,y=1) white.
  xi::Image img(10, 8, 1);
  for (int y = 1; y < 4; ++y)
    for (int x = 2; x < 6; ++x)
      img.data()[y * img.stride() + x] = 255;

  xi::Region r(img);
  VAR(w,     r.width());          // 10
  VAR(h,     r.height());         // 8
  VAR(empty, r.empty() ? 1 : 0);  // 0
  VAR(area,  xi::area(r));        // 12 (4*3)

  xi::Roi bb = xi::bbox(r);
  VAR(bx, bb.x());                // 2
  VAR(by, bb.y());                // 1
  VAR(bw, bb.w());                // 4
  VAR(bh, bb.h());                // 3

  // round-trip through cv::Mat (zero-copy view -> owning Region)
  cv::Mat m = xi::to_cv(r);
  xi::Region r2 = xi::region_from_mask(m);
  VAR(area2, xi::area(r2));       // 12
  VAR(r2_w,  r2.width());         // 10

  // NA region: empty, area 0, bbox NA
  xi::Region na = xi::Region::na("no part");
  VAR(na_empty, na.empty() ? 1 : 0);   // 1
  VAR(na_area,  xi::area(na));          // 0
  VAR(na_bbox_na, xi::bbox(na).is_na() ? 1 : 0);  // 1
}`;

async function waitRsp(c, id) { for (;;) { const m = await c.nextText(); if (m.type === 'rsp' && m.id === id) return m; } }

test('Region: binary-mask nominal type + cv helpers (area/bbox/round-trip/NA)', { skip: 'vars/preview/subscribe removed with VAR (branch refactor/remove-var-core) — pending preview plugin (observed via vars)' }, async () => {
    const dir = mkdtempSync(join(tmpdir(), 'xi_region_'));
    const sp = join(dir, 'inspect.cpp');
    writeFileSync(sp, SCRIPT);
    await withBackend(async (c) => {
        await c.nextText();
        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: slash(sp) } });
        assert.equal((await waitRsp(c, 1)).ok, true, 'compile ok (Region + xi_types_cv)');
        c.send({ type: 'cmd', id: 2, name: 'run' });
        await waitRsp(c, 2);
        let vars = null;
        for (;;) { const m = await c.nextText(); if (m.type === 'vars') { vars = m.items; break; } }
        const v = Object.fromEntries(vars.map(x => [x.name, x.value]));
        console.log('vars:', JSON.stringify(v));
        assert.equal(v.w, 10);
        assert.equal(v.h, 8);
        assert.equal(v.empty, 0, 'region not empty');
        assert.equal(v.area, 12, 'foreground pixel count');
        assert.equal(v.bx, 2, 'bbox x');
        assert.equal(v.by, 1, 'bbox y');
        assert.equal(v.bw, 4, 'bbox w');
        assert.equal(v.bh, 3, 'bbox h');
        assert.equal(v.area2, 12, 'area survives cv round-trip');
        assert.equal(v.r2_w, 10, 'dims survive cv round-trip');
        assert.equal(v.na_empty, 1, 'NA region is empty');
        assert.equal(v.na_area, 0, 'NA region area is 0');
        assert.equal(v.na_bbox_na, 1, 'bbox of NA region is NA');
    });
});
